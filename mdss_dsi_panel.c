static struct mdss_panel_data *g_pdata;

int mdss_dsi_panel_init(struct device_node *node,
	struct mdss_dsi_ctrl_pdata *ctrl_pdata,
	bool cmd_cfg_cont_splash)
{
	// ...

	g_pdata = &(ctrl_pdata->panel_data);

	return 0;
}

//////////////////////////////////////////////////////////////////////////
#include <asm/uaccess.h>
#define REGFLAG_DELAY       0xFC
#define LIC_DELAY(table)	(table->flag == REGFLAG_DELAY)
#define LIC_CMD(table)		(table->opbuf[0])
struct qcom_lcd_setting_table {
	unsigned char flag;
	int count;
	unsigned char opbuf[65];  /* opbuf[0]=cmd */
};
struct qcom_lcd_initcode {
	int total;
	struct qcom_lcd_setting_table **lines;
};

#define LCD_LINE_MAX_CHAR		1024
#define LCD_INIT_CODE_MAX_LINE	1024
#define QIS_DIGIT(x)			((x >= '0') && (x <= '9'))
#define QIS_UPCHAR(x)			((x >= 'A') && (x <= 'Z'))
#define QIS_LWCHAR(x)			((x >= 'a') && (x <= 'z'))
struct qcom_lcd_initcode g_qcomlic;

void qcom_print_lic(char *title, struct qcom_lcd_setting_table *table) 
{
	int i;

	pr_info("======= %s =======\n", title);
	pr_info("cmd: 0x%02X%s\n", LIC_CMD(table), LIC_DELAY(table) ? "(Delay)" : "");
	pr_info("count: %d\n", table->count & 0xFF);
	if (LIC_DELAY(table)) {
		return;
	}

	pr_info("data:\n");
	for (i = 1; i < table->count; i+=2) {
		pr_info("%02X, %02X\n", table->opbuf[i], table->opbuf[i+1]);
	}
	pr_info("===================\n");
}
int qcom_shex_to_int(char *hex, int length)
{
	int ret = 0;
	int i;
	char single;

	int base = 1;
	for (i = length-1; i >= 0; i--) {
		single = hex[i];
		if ((single == '\r')) {
			continue;
		}
		if ((single == 'x') || (single == 'X')){
			break;
		}
		if ((single >= '0') && (single <= '9')) {
			ret += (single - '0') * base;
		} else if ((single >= 'a') && (single <= 'f')) {
			ret += (single - 'a' + 10) * base;
		} else if((single >= 'A') && (single <= 'F')) {
			ret += (single - 'A' + 10) * base;
		} else {
			return -EINVAL;
		}
		base *= 16;
	}
	return ret;
}
/*
 * otherwise 0
 */ 
static int qcom_parse_lic_line(char *singleLine, int length) 
{
	char single;
	int i = 0;
	int ret;
	bool data = false;
	char single_byte[10] = {0};
	char bytes[65] = {0};
	int isbyte = 0;
	int ibyte = 0;
	struct qcom_lcd_setting_table *ptable = NULL;

	if (strstr(singleLine, "//")) {
		bool comment = false;
		for (i = 0; i < length; i++) {
			single = singleLine[i];
			if (single == '/') {
				comment = true;
				continue;
			}
			else if (QIS_LWCHAR(single) || QIS_UPCHAR(single)) {
				if (!comment) {
					break;
				} else {
					return 0;
				}
			}
		}
	} 
	pr_err("line(%d): %s\n", length, singleLine);
	if (strstr(singleLine, "GEN_WR(")) {
		ptable = kzalloc(sizeof(struct qcom_lcd_setting_table), GFP_KERNEL);
		for (i = 0; i < length; i++) {
			single = singleLine[i];
			if (single == '(') {
				data = true;
				continue;
			} 
			if (data) {
				if ((single == ',') || (single == ')')) {
					ret = qcom_shex_to_int(single_byte, isbyte);
					if (ret < 0) {
						pr_err("wrong data: %s\n", single_byte);
						return 0;
					}
					bytes[ibyte++] = (unsigned char)ret;
					if (single == ')') {
						break;
					}
					isbyte = 0;
					memset(single_byte, 0, 10);
				} else {
					single_byte[isbyte++] = single;
				}
			}
		}
		if (ibyte > 0) {
			ptable->count = ibyte;
			memcpy(ptable->opbuf, bytes, ptable->count);
		}
	} else if (strstr(singleLine, "delayms_pc(")) {
		ptable = kzalloc(sizeof(struct qcom_lcd_setting_table), GFP_KERNEL);
		ptable->flag = REGFLAG_DELAY;
		for (i = 0; i < length; i++) {
			single = singleLine[i];
			if (single == '(') {
				data = true;
				continue;
			} 
			if (data) {
				if (single == ')') {
					ptable->count = simple_strtoll(single_byte, NULL, 10);
					break;
				}
				single_byte[isbyte++] = single;
			}
		}
	}

	if (ptable) {
		char title[20] = {0};

		if (g_qcomlic.total == 0) { 
			if (LIC_DELAY(ptable)) {
				return 0;
			}
		}
		
		g_qcomlic.lines[g_qcomlic.total++] = ptable;

		snprintf(title, PAGE_SIZE, "data[%d]", g_qcomlic.total-1);
		qcom_print_lic(title, ptable);

	}
	return 0;
}


void qcom_get_cmds(char *filename)
{
	struct file *pcfg_file;
	char single;
	char *readbuf = NULL;
	char *singleLine = NULL;
	int ret;
	int i = 0;
	int iline = 0;
	int length = 0;
	char *filepath = NULL;
	mm_segment_t old_fs;

	filepath = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!filepath) {
		pr_err("allocate mem for cmds failed!\n");
		return;
	}
	snprintf(filepath, PATH_MAX, "/sdcard/%s", filename);
	pr_info("file path: %s\n", filepath);

	pcfg_file = filp_open(filepath, O_RDONLY, 0);

	if (IS_ERR(pcfg_file)) {
		pr_err("Open %s failed!\n", filepath);
		goto no_file_out;
	}
	old_fs = get_fs();
	set_fs(KERNEL_DS);

	readbuf = kzalloc(LCD_LINE_MAX_CHAR*2+1, GFP_KERNEL);
	singleLine = kzalloc(LCD_LINE_MAX_CHAR, GFP_KERNEL);
	if (!readbuf || !singleLine) {
		pr_err("alloca memory failed!\n");
		goto out;
	}
	g_qcomlic.lines = kzalloc(sizeof(struct qcom_lcd_initcode*) * LCD_INIT_CODE_MAX_LINE,
		GFP_KERNEL);
	if (!g_qcomlic.lines) {
		pr_err("alloca memory for g_qcomlic failed!\n");
		goto out;
	}

	while (1) {
		ret = pcfg_file->f_op->read(pcfg_file, readbuf, LCD_LINE_MAX_CHAR*2, &pcfg_file->f_pos);
		if (ret <= 0) {
			break;
		}
		length = ret;
		for (i = 0; i <= length; i++) {
			single = readbuf[i];
			if ((i == length) || (single == '\n')) {
				ret = qcom_parse_lic_line(singleLine, iline);
				if (ret < 0) {
					goto out;
				}
				iline = 0;
				memset(singleLine, 0, LCD_LINE_MAX_CHAR);
				continue;
			} else if (single != '\0') { /* wchar/char */
				singleLine[iline++] = single;
			}
		}
	}
out:
	if (!IS_ERR(pcfg_file)) {
		filp_close(pcfg_file, NULL);
		set_fs(old_fs);
	}

	kfree(singleLine);
	kfree(readbuf);
no_file_out:
	kfree(filepath);
	return;
}
/* 
 * qcom_dsi_write_file: mipi cmds registers by file
 * @filepath: file located in /sdcard
 * Return: 0 success, otherwise fail
 */
int qcom_dsi_write_file(char *filepath)
{
	struct mdss_panel_data *panel_data = g_pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = container_of(panel_data,
	struct mdss_dsi_ctrl_pdata, panel_data);
	char *filename = filepath;
	int i, i_cmd;

	struct dsi_panel_cmds qcom_pcmds;
	struct dsi_cmd_desc *cmds;

	if (filepath[0] == '/') {
		filename++;
	}
	if (g_qcomlic.total > 0) {
		for (i = 0; i < g_qcomlic.total; i++) {
			kfree(g_qcomlic.lines[i]);
		}
		kfree(g_qcomlic.lines);
		g_qcomlic.total = 0;
	}
	qcom_get_cmds(filename);

	if (g_qcomlic.total <= 0) {
		pr_err("file may be invalid!\n");
		return -EINVAL;
	}
	cmds = kzalloc(sizeof(struct dsi_cmd_desc) * g_qcomlic.total, GFP_KERNEL);
	if (cmds == NULL) {
		pr_err("alloc memory for cmds failed!\n");
		return -ENOMEM;
	}

	for (i = 0, i_cmd = 0; i < g_qcomlic.total; ) {
		struct dsi_cmd_desc *curr = &cmds[i_cmd++];
		struct qcom_lcd_setting_table *table = g_qcomlic.lines[i];

		curr->payload = table->opbuf;
		curr->dchdr.dtype = 0x29;
		curr->dchdr.last = 0x01;
		curr->dchdr.vc = 0x00;
		curr->dchdr.ack = 0x00;
		curr->dchdr.wait = 0x00;
		curr->dchdr.dlen = table->count;
		i++;
		if (i >= g_qcomlic.total) {
			break;
		}
		table = g_qcomlic.lines[i];
		if (LIC_DELAY(table)) {
			curr->dchdr.wait = table->count;
			i++;
		}
	}
	for (i = 0; i < i_cmd; i++)  {
		struct dsi_cmd_desc *curr = &cmds[i];
		pr_info("cmd[%d]: %02X(%d),delay(%d)\n", i, curr->payload[0], curr->dchdr.dlen, curr->dchdr.wait);
	}

	pr_info("total: %d, i_cmd: %d\n", g_qcomlic.total, i_cmd);
	qcom_pcmds.cmd_cnt = i_cmd;
	qcom_pcmds.link_state = DSI_LP_MODE;
	qcom_pcmds.cmds = cmds;

	mdss_dsi_panel_cmds_send(ctrl_pdata, &qcom_pcmds, CMD_REQ_COMMIT);

	kfree(cmds);
	if (g_qcomlic.total > 0) {
		for (i = 0; i < g_qcomlic.total; i++) {
			kfree(g_qcomlic.lines[i]);
		}
		kfree(g_qcomlic.lines);
		g_qcomlic.total = 0;
	}
	return 0;
}
/* 
 * qcom_dsi_write_regs: mipi write registers
 * @cmd: operate command
 * @data: data buffer to write, can be null
 * @size: data buffer size, can be 0
 * 
 * Return: 0 success, otherwise fail
 */
int qcom_dsi_write_regs(char cmd, char *data, int size)
{
	struct mdss_panel_data *panel_data = g_pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = container_of(panel_data,
					struct mdss_dsi_ctrl_pdata, panel_data);

	int i;
	struct dsi_panel_cmds qcom_pcmds;
	struct dsi_cmd_desc wcmd;
	unsigned char *opbuf;

	opbuf = kzalloc(1+size, GFP_KERNEL);
	if (!opbuf) {
		pr_err("no memory\n");
		return -ENOMEM;
	}
	opbuf[0] = cmd;
	for (i = 0; i < size; i++) {
		opbuf[i+1] = data[i];
	}

	wcmd.payload = opbuf;
	wcmd.dchdr.dtype = 0x29;
	wcmd.dchdr.last = 0x01;
	wcmd.dchdr.vc = 0x00;
	wcmd.dchdr.ack = 0x00;
	wcmd.dchdr.wait = 0x00;
	wcmd.dchdr.dlen = size+1;

	qcom_pcmds.cmd_cnt = 1;
	qcom_pcmds.link_state = DSI_LP_MODE;
	qcom_pcmds.cmds = &wcmd;

	mdss_dsi_panel_cmds_send(ctrl_pdata, &qcom_pcmds);

	kfree(opbuf);
	return 0;
}
/* 
 * qcom_dsi_read_regs: mipi read registers
 * @addr: operate register addr
 * @buffer: buffer to save return values, cannot be null
 * @size: buffer size, cannot be 0
 * 
 * Return: 0 success, otherwise fail
 */
int qcom_dsi_read_regs(char addr, char *buffer, int size)
{
	int rx_len = 0;
	int ret;

	struct mdss_panel_data *panel_data = g_pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = container_of(panel_data,
							struct mdss_dsi_ctrl_pdata, panel_data);
	
	ret = mdss_dsi_panel_cmd_read(ctrl_pdata, addr, 0x00, NULL, buffer, size);

	rx_len = ctrl_pdata->rx_len;
	
	
	if (rx_len != size) {
		pr_err("rx_len: %d, buffer_size: %d\n", rx_len, size);
		ret = -EIO;
	}

	return ret;
}