/*
 * cyttsp5_device_access.c
 * Cypress TrueTouch(TM) Standard Product V5 Device Access Module.
 * Configuration and Test command/status user interface.
 * For use with Cypress Txx5xx parts.
 * Supported parts include:
 * TMA5XX
 *
 * Copyright (C) 2012-2013 Cypress Semiconductor
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, and only version 2, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Contact Cypress Semiconductor at www.cypress.com <ttdrivers@cypress.com>
 *
 */

#include "cyttsp5_bus.h"
#include "cyttsp5_core.h"

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>
#include "cyttsp5_device_access.h"
#include "cyttsp5_regs.h"

#define CY_MAX_CONFIG_BYTES    256
#define CYTTSP5_TTHE_TUNER_GET_PANEL_DATA_FILE_NAME "get_panel_data"
#define TTHE_TUNER_MAX_BUF	(CY_MAX_PRBUF_SIZE * 3)

struct cyttsp5_device_access_data {
	struct cyttsp5_device *ttsp;
	struct cyttsp5_device_access_platform_data *pdata;
	struct cyttsp5_sysinfo *si;
	struct mutex sysfs_lock;
	u8 status;
	u16 response_length;
	bool sysfs_nodes_created;
#ifdef TTHE_TUNER_SUPPORT
	struct heatmap_param heatmap;
	struct dentry *tthe_get_panel_data_debugfs;
	struct mutex debugfs_lock;
	u8 tthe_get_panel_data_buf[TTHE_TUNER_MAX_BUF];
	u8 tthe_get_panel_data_is_open;
#endif
#ifdef VERBOSE_DEBUG
	u8 pr_buf[CY_MAX_PRBUF_SIZE];
#endif
	u8 ic_buf[CY_MAX_PRBUF_SIZE];
	u8 response_buf[CY_MAX_PRBUF_SIZE];
};

static ssize_t cyttsp5_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);
	u8 val;

	mutex_lock(&dad->sysfs_lock);
	val = dad->status;
	mutex_unlock(&dad->sysfs_lock);

	return scnprintf(buf, CY_MAX_PRBUF_SIZE, "%d\n", val);
}

static DEVICE_ATTR(status, S_IRUSR | S_IWUSR,
		   cyttsp5_status_show, NULL);

static ssize_t cyttsp5_response_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);
	int i;
	ssize_t num_read;
	int index;

	mutex_lock(&dad->sysfs_lock);
	index = scnprintf(buf, CY_MAX_PRBUF_SIZE,
			"Status %d\n", dad->status);
	if (!dad->status)
		goto error;

	num_read = dad->response_length;

	for (i = 0; i < num_read; i++)
		index += scnprintf(buf + index, CY_MAX_PRBUF_SIZE - index,
				"0x%02X\n", dad->response_buf[i]);

	index += scnprintf(buf + index, CY_MAX_PRBUF_SIZE - index,
			"(%d bytes)\n", num_read);

error:
	mutex_unlock(&dad->sysfs_lock);
	return index;
}

static DEVICE_ATTR(response, S_IRUSR | S_IWUSR,
		   cyttsp5_response_show, NULL);

/*
 * Gets user input from sysfs and parse it
 * return size of parsed output buffer
 */
static int cyttsp5_ic_parse_input(struct device *dev, const char *buf,
		size_t buf_size, u8 *ic_buf, size_t ic_buf_size)
{
	const char *pbuf = buf;
	unsigned long value;
	char scan_buf[CYTTSP5_INPUT_ELEM_SZ];
	u32 i = 0;
	u32 j;
	int last = 0;
	int ret;

	dev_dbg(dev, "%s: pbuf=%p buf=%p size=%d %s=%d buf=%s\n", __func__,
			pbuf, buf, (int) buf_size, "scan buf size",
			CYTTSP5_INPUT_ELEM_SZ, buf);

	while (pbuf <= (buf + buf_size)) {
		if (i >= CY_MAX_CONFIG_BYTES) {
			dev_err(dev, "%s: %s size=%d max=%d\n", __func__,
					"Max cmd size exceeded", i,
					CY_MAX_CONFIG_BYTES);
			return -EINVAL;
		}
		if (i >= ic_buf_size) {
			dev_err(dev, "%s: %s size=%d buf_size=%d\n", __func__,
					"Buffer size exceeded", i, ic_buf_size);
			return -EINVAL;
		}
		while (((*pbuf == ' ') || (*pbuf == ','))
				&& (pbuf < (buf + buf_size))) {
			last = *pbuf;
			pbuf++;
		}

		if (pbuf >= (buf + buf_size))
			break;

		memset(scan_buf, 0, CYTTSP5_INPUT_ELEM_SZ);
		if ((last == ',') && (*pbuf == ',')) {
			dev_err(dev, "%s: %s \",,\" not allowed.\n", __func__,
					"Invalid data format.");
			return -EINVAL;
		}
		for (j = 0; j < (CYTTSP5_INPUT_ELEM_SZ - 1)
				&& (pbuf < (buf + buf_size))
				&& (*pbuf != ' ')
				&& (*pbuf != ','); j++) {
			last = *pbuf;
			scan_buf[j] = *pbuf++;
		}

		ret = kstrtoul(scan_buf, 16, &value);
		if (ret < 0) {
			dev_err(dev, "%s: %s '%s' %s%s i=%d r=%d\n", __func__,
					"Invalid data format. ", scan_buf,
					"Use \"0xHH,...,0xHH\"", " instead.",
					i, ret);
			return ret;
		}

		ic_buf[i] = value;
		i++;
	}

	return i;
}

static ssize_t cyttsp5_command_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);
	ssize_t length;
	int rc;

	mutex_lock(&dad->sysfs_lock);
	dad->status = 0;
	dad->response_length = 0;
	length = cyttsp5_ic_parse_input(dev, buf, size, dad->ic_buf,
			CY_MAX_PRBUF_SIZE);
	if (length <= 0) {
		dev_err(dev, "%s: %s Group Data store\n", __func__,
				"Malformed input for");
		goto exit;
	}

	/* write ic_buf to log */
	cyttsp5_pr_buf(dev, dad->pr_buf, dad->ic_buf, length, "ic_buf");

	pm_runtime_get_sync(dev);

	rc = cyttsp5_request_nonhid_user_cmd(dad->ttsp, 1, CY_MAX_PRBUF_SIZE,
			dad->response_buf, length, dad->ic_buf,
			&dad->response_length);
	pm_runtime_put(dev);
	if (rc) {
		dad->response_length = 0;
		dev_err(dev, "%s: Failed to store command\n", __func__);
	} else {
		dad->status = 1;
	}

exit:
	mutex_unlock(&dad->sysfs_lock);
	dev_vdbg(dev, "%s: return size=%d\n", __func__, size);
	return size;
}

static DEVICE_ATTR(command, S_IRUSR | S_IWUSR,
	NULL, cyttsp5_command_store);

/*ZTEMT Added by luochangyang, 2014/02/21*/
/*return 0 IC detect success, return 1 failed*/
static ssize_t cyttsp5_ic_detect(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);
    int retval = 0;
	int i = 0;
	
	/*Open test*/
	cyttsp5_command_store(dev, attr, "0x04,0x00,0x05,0x00,0x2F,0x00,0x03", 35);
	msleep(50);
	if (dad->status != 1)
		retval = 1;

	for (i = 0; i < dad->response_length; i++)
		dev_info(dev, "%s: 0x%02X", __func__, dad->response_buf[i]);
	dev_info(dev, "\n");

	cyttsp5_command_store(dev, attr, "0x04,0x00,0x06,0x00,0x2F,0x00,0x26,0x03", 40);
	msleep(100);
	if (dad->status != 1)
		retval = 1;
	if (dad->response_buf[5] == 0 && dad->response_buf[6] == 0 ) {
		dev_info(dev, "%s: IC open detect success.\n", __func__);
	} else {
		dev_err(dev, "%s: IC detect failed. Sensor is open.\n", __func__);
		retval = 1;
	}

	for (i = 0; i < dad->response_length; i++)
		dev_info(dev, "%s: 0x%02X", __func__, dad->response_buf[i]);
	dev_info(dev, "\n");
#if 0
	cyttsp5_command_store(dev, attr, "0x04,0x00,0x05,0x00,0x2F,0x00,0x04", 35);
	if (dad->status != 1)
		retval = 1;

	for (i = 0; i < dad->response_length; i++)
		dev_info(dev, "%s: 0x%02X", __func__, dad->response_buf[i]);
	dev_info(dev, "\n");

	if (retval == 1)
//		goto exit;

	/*Auto short test*/
	cyttsp5_command_store(dev, attr, "0x04,0x00,0x05,0x00,0x2F,0x00,0x03", 35);
	if (dad->status != 1)
		retval = 1;

	for (i = 0; i < dad->response_length; i++)
		dev_info(dev, "%s: 0x%02X", __func__, dad->response_buf[i]);
	dev_info(dev, "\n");
#endif
	cyttsp5_command_store(dev, attr, "0x04,0x00,0x06,0x00,0x2F,0x00,0x26,0x04", 40);
	msleep(100);
	if (dad->status != 1)
		retval = 1;
	if (dad->response_buf[5] == 0 && dad->response_buf[6] == 0 ) {
		dev_info(dev, "%s: IC short detect success.\n", __func__);
	} else {
		dev_err(dev, "%s: IC detect failed. Sensor is short.\n", __func__);
		retval = 1;
	}

	for (i = 0; i < dad->response_length; i++)
		dev_info(dev, "%s: 0x%02X", __func__, dad->response_buf[i]);
	dev_info(dev, "\n");

	cyttsp5_command_store(dev, attr, "0x04,0x00,0x05,0x00,0x2F,0x00,0x04", 35);
	if (dad->status != 1)
		retval = 1;

	for (i = 0; i < dad->response_length; i++)
		dev_info(dev, "%s: 0x%02X", __func__, dad->response_buf[i]);
	dev_info(dev, "\n");

//exit:
	return snprintf(buf, PAGE_SIZE, "%d\n", retval);
}

static DEVICE_ATTR(ic_detect, 0664, cyttsp5_ic_detect, NULL);
/*ZTEMT END*/

#ifdef TTHE_TUNER_SUPPORT
/*
 * Execute scan command
 */
static int cyttsp5_exec_scan_cmd_(struct cyttsp5_device *ttsp)
{
	int rc;

	rc =  cyttsp5_request_nonhid_exec_panel_scan(ttsp, 0);
	if (rc < 0)
		dev_err(&ttsp->dev, "%s: Heatmap start scan failed r=%d\n",
			__func__, rc);
	return rc;
}

/*
 * Retrieve panel data command
 */
static int cyttsp5_ret_scan_data_cmd_(struct cyttsp5_device *ttsp,
		u16 read_offset, u16 read_count, u8 data_id, u8 *response,
		u8 *config, u16 *actual_read_len, u8 *return_buf)
{
	int rc;

	rc = cyttsp5_request_nonhid_retrieve_panel_scan(ttsp, 0, read_offset,
			read_count, data_id, response, config, actual_read_len,
			return_buf);
	if (rc < 0)
		dev_err(&ttsp->dev, "%s: Retrieve scan data failed r=%d\n",
				__func__, rc);
	return rc;
}

static ssize_t tthe_get_panel_data_debugfs_read(struct file *filp,
		char __user *buf, size_t count, loff_t *ppos)
{
	struct cyttsp5_device_access_data *dad = filp->private_data;
	struct cyttsp5_device *ttsp;
	struct device *dev;
	u8 config;
	u16 actual_read_len;
	u16 length;
	u8 element_size = 0;
	u8 *buf_offset;
	u8 *buf_out;
	int elem;
	int elem_offset = 0;
	int print_idx = 0;
	int rc;
	int rc1;
	int i;

	mutex_lock(&dad->debugfs_lock);
	ttsp = dad->ttsp;
	dev = &ttsp->dev;
	buf_out = dad->tthe_get_panel_data_buf;
	if (!buf_out)
		goto release_mutex;

	pm_runtime_get_sync(dev);

	rc = cyttsp5_request_exclusive(ttsp, CY_REQUEST_EXCLUSIVE_TIMEOUT);
	if (rc < 0)
		goto put_runtime;

	if (dad->heatmap.scan_start) {
		/* Start scan */
		rc = cyttsp5_exec_scan_cmd_(ttsp);
		if (rc < 0)
			goto release_exclusive;
	}

	elem = dad->heatmap.num_element;
	rc = cyttsp5_ret_scan_data_cmd_(ttsp, elem_offset, elem,
			dad->heatmap.data_type, dad->ic_buf, &config,
			&actual_read_len, NULL);
	if (rc < 0)
		goto release_exclusive;

	length = get_unaligned_le16(&dad->ic_buf[0]);
	buf_offset = dad->ic_buf + length;

	element_size = config & CY_CMD_RET_PANEL_ELMNT_SZ_MASK;

	elem -= actual_read_len;
	elem_offset = actual_read_len;
	while (elem > 0) {
		rc = cyttsp5_ret_scan_data_cmd_(ttsp, elem_offset, elem,
				dad->heatmap.data_type, NULL, &config,
				&actual_read_len, buf_offset);
		if (rc < 0)
			goto release_exclusive;

		if (!actual_read_len)
			break;

		length += actual_read_len * element_size;
		buf_offset = dad->ic_buf + length;
		elem -= actual_read_len;
		elem_offset += actual_read_len;
	}

	/* Reconstruct cmd header */
	put_unaligned_le16(length, &dad->ic_buf[0]);
	put_unaligned_le16(elem_offset, &dad->ic_buf[7]);

release_exclusive:
	rc1 = cyttsp5_release_exclusive(ttsp);
put_runtime:
	pm_runtime_put(dev);

	if (rc < 0)
		goto release_mutex;

	print_idx += scnprintf(buf_out, TTHE_TUNER_MAX_BUF, "CY_DATA:");
	for (i = 0; i < length; i++)
		print_idx += scnprintf(buf_out + print_idx,
				TTHE_TUNER_MAX_BUF - print_idx,
				"%02X ", dad->ic_buf[i]);
	print_idx += scnprintf(buf_out + print_idx,
			TTHE_TUNER_MAX_BUF - print_idx,
			":(%d bytes)\n", length);
	rc = simple_read_from_buffer(buf, count, ppos, buf_out, print_idx);
	print_idx = rc;

release_mutex:
	mutex_unlock(&dad->debugfs_lock);
	return print_idx;
}

static ssize_t tthe_get_panel_data_debugfs_write(struct file *filp,
		const char __user *buf, size_t count, loff_t *ppos)
{
	struct cyttsp5_device_access_data *dad = filp->private_data;
	struct device *dev = &dad->ttsp->dev;
	ssize_t length;
	int max_read;
	u8 *buf_in = dad->tthe_get_panel_data_buf;
	int ret;

	mutex_lock(&dad->debugfs_lock);
	ret = copy_from_user(buf_in + (*ppos), buf, count);
	if (ret)
		goto exit;
	buf_in[count] = 0;

	length = cyttsp5_ic_parse_input(dev, buf_in, count, dad->ic_buf,
			CY_MAX_PRBUF_SIZE);
	if (length <= 0) {
		dev_err(dev, "%s: %s Group Data store\n", __func__,
				"Malformed input for");
		goto exit;
	}

	/* update parameter value */
	dad->heatmap.num_element = get_unaligned_le16(&dad->ic_buf[3]);
	dad->heatmap.data_type = dad->ic_buf[5];

	if (dad->ic_buf[6] > 0)
		dad->heatmap.scan_start = true;
	else
		dad->heatmap.scan_start = false;

	/* elem can not be bigger then buffer size */
	max_read = CY_CMD_RET_PANEL_HDR;
	max_read += dad->heatmap.num_element * CY_CMD_RET_PANEL_ELMNT_SZ_MAX;

	if (max_read >= CY_MAX_PRBUF_SIZE) {
		dad->heatmap.num_element =
			(CY_MAX_PRBUF_SIZE - CY_CMD_RET_PANEL_HDR)
			/ CY_CMD_RET_PANEL_ELMNT_SZ_MAX;
		dev_err(dev, "%s: Will get %d element\n", __func__,
				dad->heatmap.num_element);
	}

exit:
	mutex_unlock(&dad->debugfs_lock);
	dev_vdbg(dev, "%s: return count=%d\n", __func__, count);
	return count;
}

static int tthe_get_panel_data_debugfs_open(struct inode *inode,
		struct file *filp)
{
	struct cyttsp5_device_access_data *dad = inode->i_private;

	mutex_lock(&dad->debugfs_lock);

	if (dad->tthe_get_panel_data_is_open) {
		mutex_unlock(&dad->debugfs_lock);
		return -EBUSY;
	}

	filp->private_data = inode->i_private;

	dad->tthe_get_panel_data_is_open = 1;
	mutex_unlock(&dad->debugfs_lock);
	return 0;
}

static int tthe_get_panel_data_debugfs_close(struct inode *inode,
		struct file *filp)
{
	struct cyttsp5_device_access_data *dad = filp->private_data;

	mutex_lock(&dad->debugfs_lock);
	filp->private_data = NULL;
	dad->tthe_get_panel_data_is_open = 0;
	mutex_unlock(&dad->debugfs_lock);

	return 0;
}

static const struct file_operations tthe_get_panel_data_fops = {
	.open = tthe_get_panel_data_debugfs_open,
	.release = tthe_get_panel_data_debugfs_close,
	.read = tthe_get_panel_data_debugfs_read,
	.write = tthe_get_panel_data_debugfs_write,
};
#endif

#ifdef CONFIG_PM_SLEEP
static int cyttsp5_device_access_suspend(struct device *dev)
{
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);

	if (!mutex_trylock(&dad->sysfs_lock))
		return -EBUSY;

	mutex_unlock(&dad->sysfs_lock);
	return 0;
}

static int cyttsp5_device_access_resume(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops cyttsp5_device_access_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cyttsp5_device_access_suspend,
			cyttsp5_device_access_resume)
};

static int cyttsp5_setup_sysfs(struct cyttsp5_device *ttsp)
{
	struct device *dev = &ttsp->dev;
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);
	int rc = 0;

	/*ZTEMT Added by luochangyang, 2014/02/21*/
	rc = device_create_file(dev, &dev_attr_ic_detect);
	if (rc) {
		dev_err(dev, "%s: Error, could not create ic_detect\n",
				__func__);
		goto exit;
	}
	/*ZTEMT END*/

	rc = device_create_file(dev, &dev_attr_command);
	if (rc) {
		dev_err(dev, "%s: Error, could not create command\n",
				__func__);
		goto unregister_ic_detect;
	}

	rc = device_create_file(dev, &dev_attr_status);
	if (rc) {
		dev_err(dev, "%s: Error, could not create status\n",
				__func__);
		goto unregister_command;
	}

	rc = device_create_file(dev, &dev_attr_response);
	if (rc) {
		dev_err(dev, "%s: Error, could not create response\n",
				__func__);
		goto unregister_status;
	}

#ifdef TTHE_TUNER_SUPPORT
	dad->tthe_get_panel_data_debugfs = debugfs_create_file(
			CYTTSP5_TTHE_TUNER_GET_PANEL_DATA_FILE_NAME,
			0644, NULL, dad, &tthe_get_panel_data_fops);
	if (IS_ERR_OR_NULL(dad->tthe_get_panel_data_debugfs)) {
		dev_err(dev, "%s: Error, could not create get_panel_data\n",
				__func__);
		dad->tthe_get_panel_data_debugfs = NULL;
		goto unregister_response;
	}
#endif

	dad->sysfs_nodes_created = true;
	return rc;

#ifdef TTHE_TUNER_SUPPORT
unregister_response:
	device_remove_file(dev, &dev_attr_response);
#endif
unregister_status:
	device_remove_file(dev, &dev_attr_status);
unregister_command:
	device_remove_file(dev, &dev_attr_command);
unregister_ic_detect:
	device_remove_file(dev, &dev_attr_ic_detect);
exit:
	return rc;
}

static int cyttsp5_setup_sysfs_attention(struct cyttsp5_device *ttsp)
{
	struct device *dev = &ttsp->dev;
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);
	int rc = 0;

	dad->si = cyttsp5_request_sysinfo(ttsp);
	if (!dad->si)
		return -1;

	rc = cyttsp5_setup_sysfs(ttsp);

	cyttsp5_unsubscribe_attention(ttsp, CY_ATTEN_STARTUP,
		cyttsp5_setup_sysfs_attention, 0);

	return rc;

}

static int cyttsp5_device_access_probe(struct cyttsp5_device *ttsp)
{
	struct device *dev = &ttsp->dev;
	struct cyttsp5_device_access_data *dad;
	struct cyttsp5_device_access_platform_data *pdata =
			dev_get_platdata(dev);
	int rc = 0;

	dad = kzalloc(sizeof(*dad), GFP_KERNEL);
	if (dad == NULL) {
		dev_err(dev, "%s: Error, kzalloc\n", __func__);
		rc = -ENOMEM;
		goto cyttsp5_device_access_probe_data_failed;
	}

	mutex_init(&dad->sysfs_lock);
	dad->ttsp = ttsp;
	dad->pdata = pdata;
	dev_set_drvdata(dev, dad);
#ifdef TTHE_TUNER_SUPPORT
	mutex_init(&dad->debugfs_lock);
	dad->heatmap.num_element = 200;
#endif

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	/* get sysinfo */
	dad->si = cyttsp5_request_sysinfo(ttsp);
	pm_runtime_put(dev);
	if (dad->si) {
		rc = cyttsp5_setup_sysfs(ttsp);
		if (rc)
			goto cyttsp5_device_access_setup_sysfs_failed;
	} else {
		dev_err(dev, "%s: Fail get sysinfo pointer from core p=%p\n",
				__func__, dad->si);
		cyttsp5_subscribe_attention(ttsp, CY_ATTEN_STARTUP,
			cyttsp5_setup_sysfs_attention, 0);
	}

	return 0;

 cyttsp5_device_access_setup_sysfs_failed:
	pm_runtime_suspend(dev);
	pm_runtime_disable(dev);
	dev_set_drvdata(dev, NULL);
	kfree(dad);
 cyttsp5_device_access_probe_data_failed:
	dev_err(dev, "%s failed.\n", __func__);
	return rc;
}

static int cyttsp5_device_access_release(struct cyttsp5_device *ttsp)
{
	struct device *dev = &ttsp->dev;
	struct cyttsp5_device_access_data *dad = dev_get_drvdata(dev);

	pm_runtime_suspend(dev);
	pm_runtime_disable(dev);

	if (dad->sysfs_nodes_created) {
		device_remove_file(dev, &dev_attr_command);
		device_remove_file(dev, &dev_attr_status);
		device_remove_file(dev, &dev_attr_response);
#ifdef TTHE_TUNER_SUPPORT
		debugfs_remove(dad->tthe_get_panel_data_debugfs);
#endif
	} else {
		cyttsp5_unsubscribe_attention(ttsp, CY_ATTEN_STARTUP,
			cyttsp5_setup_sysfs_attention, 0);
	}

	dev_set_drvdata(dev, NULL);
	kfree(dad);
	return 0;
}

static struct cyttsp5_driver cyttsp5_device_access_driver = {
	.probe = cyttsp5_device_access_probe,
	.remove = cyttsp5_device_access_release,
	.driver = {
		.name = CYTTSP5_DEVICE_ACCESS_NAME,
		.bus = &cyttsp5_bus_type,
		.owner = THIS_MODULE,
		.pm = &cyttsp5_device_access_pm_ops,
	},
};

static struct cyttsp5_device_access_platform_data
	_cyttsp5_device_access_platform_data = {
	.device_access_dev_name = CYTTSP5_DEVICE_ACCESS_NAME,
};

static const char cyttsp5_device_access_name[] = CYTTSP5_DEVICE_ACCESS_NAME;
static struct cyttsp5_device_info
	cyttsp5_device_access_infos[CY_MAX_NUM_CORE_DEVS];

static char *core_ids[CY_MAX_NUM_CORE_DEVS] = {
	CY_DEFAULT_CORE_ID,
	NULL,
	NULL,
	NULL,
	NULL
};

static int num_core_ids = 1;

module_param_array(core_ids, charp, &num_core_ids, 0);
MODULE_PARM_DESC(core_ids,
	"Core id list of cyttsp5 core devices for device access module");

static int __init cyttsp5_device_access_init(void)
{
	int rc = 0;
	int i, j;

	/* Check for invalid or duplicate core_ids */
	for (i = 0; i < num_core_ids; i++) {
		if (!strlen(core_ids[i])) {
			pr_err("%s: core_id %d is empty\n",
				__func__, i+1);
			return -EINVAL;
		}
		for (j = i+1; j < num_core_ids; j++)
			if (!strcmp(core_ids[i], core_ids[j])) {
				pr_err("%s: core_ids %d and %d are same\n",
					__func__, i+1, j+1);
				return -EINVAL;
			}
	}

	for (i = 0; i < num_core_ids; i++) {
		cyttsp5_device_access_infos[i].name =
			cyttsp5_device_access_name;
		cyttsp5_device_access_infos[i].core_id = core_ids[i];
		cyttsp5_device_access_infos[i].platform_data =
			&_cyttsp5_device_access_platform_data;
		pr_info("%s: Registering device access device for core_id: %s\n",
			__func__, cyttsp5_device_access_infos[i].core_id);
		rc = cyttsp5_register_device(&cyttsp5_device_access_infos[i]);
		if (rc < 0) {
			pr_err("%s: Error, failed registering device\n",
				__func__);
			goto fail_unregister_devices;
		}
	}
	rc = cyttsp5_register_driver(&cyttsp5_device_access_driver);
	if (rc) {
		pr_err("%s: Error, failed registering driver\n", __func__);
		goto fail_unregister_devices;
	}

	pr_info("%s: Cypress TTSP Device Access Driver (Built %s) rc=%d\n",
		 __func__, CY_DRIVER_DATE, rc);
	return 0;

fail_unregister_devices:
	for (i--; i <= 0; i--) {
		cyttsp5_unregister_device(cyttsp5_device_access_infos[i].name,
			cyttsp5_device_access_infos[i].core_id);
		pr_info("%s: Unregistering device access device for core_id: %s\n",
			__func__, cyttsp5_device_access_infos[i].core_id);
	}
	return rc;
}
module_init(cyttsp5_device_access_init);

static void __exit cyttsp5_device_access_exit(void)
{
	int i;

	cyttsp5_unregister_driver(&cyttsp5_device_access_driver);
	for (i = 0; i < num_core_ids; i++) {
		cyttsp5_unregister_device(cyttsp5_device_access_infos[i].name,
			cyttsp5_device_access_infos[i].core_id);
		pr_info("%s: Unregistering device access device for core_id: %s\n",
			__func__, cyttsp5_device_access_infos[i].core_id);
	}
}
module_exit(cyttsp5_device_access_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cypress TrueTouch(R) Standard Product Device Access Driver");
MODULE_AUTHOR("Cypress Semiconductor <ttdrivers@cypress.com>");
