/*
 * The Wi-Fi of the POCO X3 NFC (surya): a Qualcomm WCN3990, whose firmware runs on the modem (in its user domain
 * msm/modem/wlan_pd, which the modem starts from wlanmdsp.mbn once its registry and TFTP are answered: drivers/modem/
 * hakios.mpss.c). Step one of its driver: the exchange with that firmware's service (WLFW, 69), in the order and with
 * the values of Linux's ath10k (drivers/net/wireless/ath/ath10k/qmi.c, ath10k_qmi_event_server_arrive() and what follows
 * "MSA ready"):
 *
 *   1  which indications we want (firmware ready, MSA ready); the firmware's status
 *   2  what the host can do (daemon support 0); a firmware that does not know the message is no matter, as in ath10k
 *   3  the MSA memory: where it is (surya: reserved-memory wlan_msa_region@97500000, qcom,wlan-msa-memory 1 MB); the
 *      firmware answers with its regions, which TrustZone then gives to the modem and the WLAN (and the WLAN's copy
 *      engines, for a region that is not secure), from us (qcom_scm_assign_mem), 20 ms after the answer as ath10k waits
 *   4  "MSA ready"
 *   5  what the firmware and the chip are: chip id and family, board id, soc id, firmware version and build
 *   6  the board's data file in pieces of 6144 bytes (bdwlan.bXX for board id XX, else bdwlan.bin: the phone's own,
 *      from its firmware partition's image/, /fw/image on the root with MODEM=1 userland/build.sh); a CRC complaint at
 *      the last piece is no matter, as in ath10k
 *   7  the calibration report (none)
 *   8  "firmware ready" waited for (wcn3990_wait seconds, default 15)
 *
 * Seen on the phone (3 November 2026): every request answered with success, the MSA memory in two regions (16 KB secure at
 * 0x97500000, the rest not) given by TrustZone, chip id 0x30214 (family 0x4001), no board id (so bdwlan.bin, 26 KB, in 5
 * pieces), firmware WLAN.HL.3.2.2-00650 of 15 August 2021, and "firmware ready" 3.9 s after the exchange began.
 *
 * Nothing after that: the mode, the copy engines' configuration and the radio's driver (ath10k's snoc.c) are the steps to
 * come. The chip's rails (vdd-1.8-xo, vdd-1.3-rfa, vdd-3.3-ch0/1, vdd-cx-mx) are as the boot loader leaves them.
 *
 * boot-arg wcn3990=1 runs it, on a thread of its own, once the modem's QRTR is up (modem_sm7150=1 modem=1, or
 * modem_step=10 modem_keep=1); the MSA memory is given back to us when the modem is shut down (hakios_wcn3990_modem_down()).
 *
 * The messages' structures and tables are ath10k's own (qmi_wlfw_v01.c and .h, ISC), taken from Linux's sources when the
 * kernel is built (tools/mk-wlfw10.sh) and not kept in the repository. Without them this compiles to a line that says so.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

void hakios_wcn3990_modem_down(void);
/* drivers/modem/hakios.mpss.c: its QRTR, and TrustZone's memory assignment (read and write to each; 0, or < 0) */
const struct hakios_qrtr_ops *hakios_mpss_qrtr(void);
int hakios_mpss_assign(uint64_t address, uint64_t size, const uint32_t *sources, unsigned source_count,
    const uint32_t *destinations, unsigned destination_count);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#if !__has_include("hakios.wlfw_ath10k.h")

void
hakios_wcn3990_modem_down(void)
{
}

static void
wcn3990_init(void)
{
	uint32_t on = 0;

	if (PE_parse_boot_argn("wcn3990", &on, sizeof(on)) && on) {
		say("wcn3990: THE EXCHANGE IS NOT BUILT IN: ath10k's message descriptions were not there when this kernel was built "
		    "(tools/mk-wlfw10.sh, which tools/make-kernel.sh runs when it can fetch them)\n");
	}
}

#else

#define HAKIOS_QMI_NO_KERNEL_CONNECT
#include "hakios.wlfw_ath10k.h"

#define MSA_ADDRESS             0x97500000ull   /* dt: reserved-memory wlan_msa_region@97500000 (qcom,wlan-msa-fixed-region) */
#define MSA_SIZE                0x00100000u     /* dt: qcom,wlan-msa-memory of qcom,icnss@18800000 */
#define CLIENT_ID               0x4b4e454c      /* ath10k's ATH10K_QMI_CLIENT_ID */
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define VMID_WLAN               0x18
#define VMID_WLAN_CE            0x19
#define TIMEOUT_MS              30000           /* ath10k's ATH10K_QMI_TIMEOUT, 30 s */
#define RESPONSE_MAX            8192

static struct qmi_handle handle;
static struct {
	volatile bool server, gone, fw_ready, msa_ready;
	uint64_t began;
} wlfw;
static struct {
	uint64_t address;
	uint32_t size;
	bool secure;
} regions[QMI_WLFW_MAX_MEM_REG_V01];
static unsigned region_count;
static volatile bool regions_ours_again = true;

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000000;
}

static int
new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = hakios_qmi_connect(qmi->sock, service->node, service->port);

	say("wcn3990: +%llu ms: the firmware's service is at node %u port %u (service %#x, version %u, instance %u)%s\n",
	    now_ms() - wlfw.began, service->node, service->port, service->service, service->version, service->instance,
	    error ? ": CANNOT BE CONNECTED TO" : "");
	wlfw.server = error == 0;
	return error;
}

static void
del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	say("wcn3990: THE FIRMWARE'S SERVICE AT NODE %u PORT %u HAS GONE\n", service->node, service->port);
	wlfw.gone = true;
}

static void
fw_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	say("wcn3990: +%llu ms: THE FIRMWARE INDICATES \"FIRMWARE READY\"\n", now_ms() - wlfw.began);
	wlfw.fw_ready = true;
}

static void
msa_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	say("wcn3990: +%llu ms: the firmware indicates \"MSA ready\"\n", now_ms() - wlfw.began);
	wlfw.msa_ready = true;
}

static const struct qmi_ops ops = { .new_server = new_server, .del_server = del_server };
static const struct qmi_msg_handler handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, wlfw_fw_ready_ind_msg_v01_ei, sizeof(struct wlfw_fw_ready_ind_msg_v01),
	  fw_ready },
	{ QMI_INDICATION, QMI_WLFW_MSA_READY_IND_V01, wlfw_msa_ready_ind_msg_v01_ei, sizeof(struct wlfw_msa_ready_ind_msg_v01),
	  msa_ready },
	{ 0 },
};

/*
 * One request and its reply, as each of ath10k's goes; `reply`'s first field is the result. `fine` is an error that is no
 * matter (0: none). Says what was asked, the answer, and how long it took. True: success (or the error that is no matter).
 */
static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply, uint16_t fine, bool quiet)
{
	const struct qmi_response_type_v01 *result = reply;
	struct qmi_txn txn;
	uint64_t began = now_ms();
	int ret = qmi_txn_init(&handle, &txn, reply_ei, reply);

	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = hakios_qmi_txn_wait_ms(&txn, TIMEOUT_MS);
		}
	}
	if (ret < 0) {
		say("wcn3990: %s (message %#x): NO ANSWER THAT CAN BE READ: %d, after %llu ms\n", what, message, ret, now_ms() - began);
		return false;
	}
	if (result->result == QMI_RESULT_SUCCESS_V01) {
		if (!quiet) {
			say("wcn3990: %s (message %#x): success, after %llu ms\n", what, message, now_ms() - began);
		}
		return true;
	}
	if (fine != 0 && result->error == fine) {
		say("wcn3990: %s (message %#x): error %u, which ath10k lets pass\n", what, message, result->error);
		return true;
	}
	say("wcn3990: %s (message %#x): THE FIRMWARE REFUSES: result %u, error %u, after %llu ms\n", what, message, result->result,
	    result->error, now_ms() - began);
	return false;
}

static bool
wait_for(volatile bool *flag, uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !*flag && !wlfw.gone; i++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	return *flag;
}

/* ath10k_qmi_map_msa_permission() for each region the firmware named: from us to the modem and the WLAN (and its CE). */
static bool
msa_to_firmware(void)
{
	static const uint32_t from[1] = { VMID_HLOS };

	for (unsigned i = 0; i < region_count; i++) {
		uint32_t to[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
		unsigned count = regions[i].secure ? 2 : 3;
		int ret = hakios_mpss_assign(regions[i].address, regions[i].size, from, 1, to, count);

		say("wcn3990: TrustZone asked to give region %u (%#llx, %#x bytes) to the modem, the WLAN%s: %s (%d)\n", i,
		    regions[i].address, regions[i].size, count == 3 ? " and its copy engines" : "", ret == 0 ? "DONE" : "REFUSED", ret);
		if (ret != 0) {
			return false;
		}
		regions_ours_again = false;
	}
	return true;
}

/* ath10k_qmi_unmap_msa_permission(): back to us, once the modem is down (or when the exchange gives up). */
void
hakios_wcn3990_modem_down(void)
{
	static const uint32_t to[1] = { VMID_HLOS };

	if (regions_ours_again) {
		return;
	}
	for (unsigned i = 0; i < region_count; i++) {
		uint32_t from[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };
		int ret = hakios_mpss_assign(regions[i].address, regions[i].size, from, regions[i].secure ? 2 : 3, to, 1);

		say("wcn3990: the MSA region %u given back to us: %s (%d)\n", i, ret == 0 ? "done" : "REFUSED", ret);
	}
	regions_ours_again = true;
}

/* The board's data, as ath10k_qmi_bdf_dnld_send_sync() sends it: file id 0, pieces of 6144, "end" on the last. */
static bool
board_data(uint32_t board_id)
{
	static struct wlfw_bdf_download_req_msg_v01 request;
	struct wlfw_bdf_download_resp_msg_v01 reply;
	char name[32];
	uint64_t size = 0, began = now_ms();
	const uint8_t *data = NULL, *at;
	uint32_t left, pieces = 0;
	bool ok = true;

	if (board_id != 0xff) {
		snprintf(name, sizeof(name), "image/bdwlan.b%02x", board_id & 0xff);
		data = hakios_adsp_fw_file(name, &size);
	}
	if (data == NULL) {
		strlcpy(name, "image/bdwlan.bin", sizeof(name));
		data = hakios_adsp_fw_file(name, &size);
	}
	if (data == NULL || size == 0 || size > 1024 * 1024) {
		say("wcn3990: the board's data: no /fw/image/bdwlan.b%02x nor bdwlan.bin on the root (pull-firmware.sh, then MODEM=1 "
		    "SYSTEM=1 userland/build.sh)\n", board_id & 0xff);
		if (data != NULL) {
			hakios_adsp_fw_free(data, size);
		}
		return false;
	}
	say("wcn3990: the board's data: fw/%s, %llu bytes, beginning %02x %02x %02x %02x\n", name, size, data[0], data[1], data[2],
	    data[3]);
	bzero(&request, sizeof(request));
	for (at = data, left = (uint32_t)size; left > 0; pieces++) {
		request.valid = 1;
		request.file_id_valid = 1;
		request.file_id = 0;
		request.total_size_valid = 1;
		request.total_size = (uint32_t)size;
		request.seg_id_valid = 1;
		request.data_valid = 1;
		request.end_valid = 1;
		request.end = left <= QMI_WLFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, at, request.data_len);
		bzero(&reply, sizeof(reply));
		/* (end = 1 has the firmware check the file's CRC: a "malformed" then is no matter, ath10k says) */
		if (!ask("the board's data", QMI_WLFW_BDF_DOWNLOAD_REQ_V01, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_bdf_download_req_msg_v01_ei, &request, wlfw_bdf_download_resp_msg_v01_ei, &reply,
		    request.end ? QMI_ERR_MALFORMED_MSG_V01 : 0, true)) {
			say("wcn3990: the board's data: REFUSED AT PIECE %u (%u bytes, %u still to go)\n", pieces, request.data_len, left);
			ok = false;
			break;
		}
		left -= request.data_len;
		at += request.data_len;
		request.seg_id++;
	}
	if (ok) {
		say("wcn3990: the board's data: ACCEPTED, %u pieces in %llu ms\n", pieces, now_ms() - began);
	}
	hakios_adsp_fw_free(data, size);
	return ok;
}

static void
exchange(void)
{
	struct wlfw_ind_register_req_msg_v01 indications;
	struct wlfw_ind_register_resp_msg_v01 indications_reply;
	struct wlfw_host_cap_req_msg_v01 host;
	struct wlfw_host_cap_resp_msg_v01 host_reply;
	struct wlfw_msa_info_req_msg_v01 msa;
	static struct wlfw_msa_info_resp_msg_v01 msa_reply;
	struct wlfw_msa_ready_req_msg_v01 ready = { 0 };
	struct wlfw_msa_ready_resp_msg_v01 ready_reply;
	struct wlfw_cap_req_msg_v01 what = { 0 };
	static struct wlfw_cap_resp_msg_v01 target;
	static struct wlfw_cal_report_req_msg_v01 report;
	struct wlfw_cal_report_resp_msg_v01 report_reply;
	uint32_t board_id, seconds = 15;
	int ret;

	PE_parse_boot_argn("wcn3990_wait", &seconds, sizeof(seconds));
	ret = qmi_handle_init(&handle, RESPONSE_MAX, &ops, handlers);
	if (ret < 0) {
		say("wcn3990: no handle on the modem's router: %d\n", ret);
		return;
	}
	ret = qmi_add_lookup(&handle, WLFW_SERVICE_ID_V01, WLFW_SERVICE_VERS_V01, 0);
	if (ret < 0 || !wait_for(&wlfw.server, 120000)) {
		say("wcn3990: THE FIRMWARE'S SERVICE (%#x, version %u) DOES NOT APPEAR within 120 s (lookup: %d)\n", WLFW_SERVICE_ID_V01,
		    WLFW_SERVICE_VERS_V01, ret);
		goto out;
	}

	/* 1: ath10k_qmi_ind_register_send_sync_msg() */
	bzero(&indications, sizeof(indications));
	bzero(&indications_reply, sizeof(indications_reply));
	indications.client_id_valid = 1;
	indications.client_id = CLIENT_ID;
	indications.fw_ready_enable_valid = 1;
	indications.fw_ready_enable = 1;
	indications.msa_ready_enable_valid = 1;
	indications.msa_ready_enable = 1;
	if (!ask("1: which indications we want", QMI_WLFW_IND_REGISTER_REQ_V01, WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_req_msg_v01_ei, &indications, wlfw_ind_register_resp_msg_v01_ei, &indications_reply, 0, false)) {
		goto out;
	}
	if (indications_reply.fw_status_valid) {
		say("wcn3990: 1:   the firmware's status: %#llx%s\n", (unsigned long long)indications_reply.fw_status,
		    (indications_reply.fw_status & QMI_WLFW_FW_READY_V01) ? ": READY ALREADY (a run before this one set it up)" : "");
		if (indications_reply.fw_status & QMI_WLFW_FW_READY_V01) {
			wlfw.fw_ready = true;
			goto out;
		}
	}

	/* 2: ath10k_qmi_host_cap_send_sync() */
	bzero(&host, sizeof(host));
	bzero(&host_reply, sizeof(host_reply));
	host.daemon_support_valid = 1;
	host.daemon_support = 0;
	if (!ask("2: what the host can do", QMI_WLFW_HOST_CAP_REQ_V01, WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_host_cap_req_msg_v01_ei, &host, wlfw_host_cap_resp_msg_v01_ei, &host_reply, QMI_ERR_NOT_SUPPORTED_V01, false)) {
		goto out;
	}

	/* 3: ath10k_qmi_msa_mem_info_send_sync_msg(), then the regions given to the firmware */
	bzero(&msa, sizeof(msa));
	bzero(&msa_reply, sizeof(msa_reply));
	msa.msa_addr = MSA_ADDRESS;
	msa.size = MSA_SIZE;
	if (!ask("3: where the MSA memory is", QMI_WLFW_MSA_INFO_REQ_V01, WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_msa_info_req_msg_v01_ei, &msa, wlfw_msa_info_resp_msg_v01_ei, &msa_reply, 0, false)) {
		goto out;
	}
	if (msa_reply.mem_region_info_len > QMI_WLFW_MAX_MEM_REG_V01) {
		say("wcn3990: 3:   %u regions: more than %u\n", msa_reply.mem_region_info_len, QMI_WLFW_MAX_MEM_REG_V01);
		goto out;
	}
	region_count = 0;
	for (unsigned i = 0; i < msa_reply.mem_region_info_len; i++) {
		const struct wlfw_memory_region_info_s_v01 *r = &msa_reply.mem_region_info[i];

		say("wcn3990: 3:   region %u: %#llx, %#x bytes, %s\n", i, (unsigned long long)r->region_addr, r->size,
		    r->secure_flag ? "secure" : "not secure");
		if (r->size > MSA_SIZE || r->region_addr < MSA_ADDRESS || r->region_addr + r->size > MSA_ADDRESS + MSA_SIZE) {
			say("wcn3990: 3:   NOT WITHIN THE MSA MEMORY: given up, as ath10k gives up\n");
			goto out;
		}
		regions[i].address = r->region_addr;
		regions[i].size = r->size;
		regions[i].secure = r->secure_flag != 0;
		region_count++;
	}
	delay_for_interval(20, NSEC_PER_MSEC);  /* (ath10k: "to prevent SDM845 from crashing due to a security violation") */
	if (!msa_to_firmware()) {
		goto out;
	}

	/* 4: ath10k_qmi_msa_ready_send_sync_msg() */
	bzero(&ready_reply, sizeof(ready_reply));
	if (!ask("4: MSA ready", QMI_WLFW_MSA_READY_REQ_V01, WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN, wlfw_msa_ready_req_msg_v01_ei,
	    &ready, wlfw_msa_ready_resp_msg_v01_ei, &ready_reply, 0, false)) {
		goto out;
	}

	/* 5: ath10k_qmi_cap_send_sync_msg() */
	bzero(&target, sizeof(target));
	if (!ask("5: what the firmware and the chip are", QMI_WLFW_CAP_REQ_V01, WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_cap_req_msg_v01_ei, &what, wlfw_cap_resp_msg_v01_ei, &target, 0, false)) {
		goto out;
	}
	board_id = target.board_info_valid ? target.board_info.board_id : 0xff;
	say("wcn3990: 5:   CHIP ID %#x, family %#x (%s); BOARD ID %#x (%s); soc id %#x (%s)\n", target.chip_info.chip_id,
	    target.chip_info.chip_family, target.chip_info_valid ? "given" : "not given", board_id,
	    target.board_info_valid ? "given" : "not given: 0xff", target.soc_info.soc_id, target.soc_info_valid ? "given" : "not given");
	say("wcn3990: 5:   FIRMWARE %#x, built \"%s\" (%s); build id \"%s\" (%s)\n", target.fw_version_info.fw_version,
	    target.fw_version_info.fw_build_timestamp, target.fw_version_info_valid ? "given" : "not given", target.fw_build_id,
	    target.fw_build_id_valid ? "given" : "not given");

	/* ath10k goes on when the firmware indicates "MSA ready" (it may have come already) */
	if (!wait_for(&wlfw.msa_ready, 5000)) {
		say("wcn3990: no \"MSA ready\" indication within 5 s: going on, as the answer to 4 was success\n");
	}

	/* 6: ath10k_qmi_bdf_dnld_send_sync() */
	if (!board_data(board_id)) {
		goto out;
	}

	/* 7: ath10k_qmi_send_cal_report_req(): no calibration data of ours */
	bzero(&report, sizeof(report));
	bzero(&report_reply, sizeof(report_reply));
	report.meta_data_len = 0;
	if (!ask("7: the calibration report (none)", QMI_WLFW_CAL_REPORT_REQ_V01, WLFW_CAL_REPORT_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_cal_report_req_msg_v01_ei, &report, wlfw_cal_report_resp_msg_v01_ei, &report_reply, 0, false)) {
		goto out;
	}

	/* 8: "firmware ready" */
	if (!wait_for(&wlfw.fw_ready, seconds * 1000)) {
		say("wcn3990: 8: NO \"FIRMWARE READY\" within %u s of the calibration report\n", seconds);
	}
out:
	say("wcn3990: the exchange ends %llu ms after it began: \"MSA ready\" %s, \"FIRMWARE READY\" %s\n", now_ms() - wlfw.began,
	    wlfw.msa_ready ? "yes" : "no", wlfw.fw_ready ? "YES" : "no");
}

static void
wcn3990_thread(__unused void *parameter, __unused wait_result_t result)
{
	const struct hakios_qrtr_ops *qrtr = NULL;
	unsigned waited;

	/* the modem's router, once its link is up (it comes a few seconds after the modem, which waits for the root) */
	for (waited = 0; waited < 600 && (qrtr = hakios_mpss_qrtr()) == NULL; waited++) {
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	if (qrtr == NULL) {
		say("wcn3990: the modem's QRTR is not up after ten minutes (modem_sm7150=1 and modem=1, or modem_step=10 "
		    "modem_keep=1?): stopping\n");
		thread_terminate(current_thread());
	}
	say("wcn3990: the modem's QRTR is up (after %u s): the exchange with the WCN3990's firmware, as ath10k's qmi.c makes it\n",
	    waited);
	hakios_qmi_set_transport(qrtr);
	wlfw.began = now_ms();
	exchange();
	thread_terminate(current_thread());
}

static void
wcn3990_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("wcn3990", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(wcn3990_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

#endif

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, wcn3990_init);
