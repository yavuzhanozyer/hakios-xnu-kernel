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
 *   9  (wcn3990=2) the configuration: snoc.c's copy engine pipes, the map of HTC services to them, the shadow registers
 *  10  (wcn3990=2) the mode "mission": the radio on, waiting for its driver at the copy engines
 *  13  (wcn3990=5) WMI's init (the firmware's memory requests met from the compute DSP's region at 0x93c00000), its
 *      ready (the radio's MAC address), a station's vdev, the channels, and a scan; every network heard, listed
 *  12  (wcn3990=4) HTC: the WMI control service connected, setup complete, and the firmware's first WMI events
 *  11  (wcn3990=3) the copy engines: the WLAN's stream through the SMMU, three engines' rings, buffers to receive into,
 *      and the firmware's first HTC message ("ready") watched for (see copy_engines())
 *
 * Seen on the phone (5 November 2026, wcn3990=4): HTC's "ready" on CE2 (2 credits of 2184 bytes); WMI's control service
 * connected (endpoint 1, messages of up to 2040 bytes); after the setup, WMI's service available (3), SERVICE READY (1, 332
 * bytes, the firmware's version in it), and events 0x4009 and 0x4022.
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
#include <kern/locks.h>
#include <vm/memory_types.h>
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
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern lck_mtx_t hakios_smmu_lock;      /* the SMMU's match registers, taken by the sound and the Wi-Fi as well */

void hakios_wcn3990_modem_down(void);
/* drivers/modem/hakios.mpss.c: its QRTR, and TrustZone's memory assignment (read and write to each; 0, or < 0) */
const struct hakios_qrtr_ops *hakios_mpss_qrtr(void);
int hakios_mpss_assign(uint64_t address, uint64_t size, const uint32_t *sources, unsigned source_count,
    const uint32_t *destinations, unsigned destination_count);
int hakios_mpss_regulator(const char *name, uint32_t millivolts, uint32_t mode, bool on);

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

/*
 * ath10k_snoc_wlan_enable() and ath10k_qmi_wlan_enable(): the configuration -- the copy engines' pipes, which HTC service
 * goes through which, the shadow registers, snoc.c's three tables for the WCN3990 -- then the mode "mission", after which
 * the firmware has the radio on and waits for its driver at the copy engines (the steps to come). wcn3990=2.
 */
static bool in_mission;

static bool
mission(void)
{
	static struct wlfw_wlan_cfg_req_msg_v01 config;
	struct wlfw_wlan_cfg_resp_msg_v01 config_reply;
	struct wlfw_wlan_mode_req_msg_v01 mode;
	struct wlfw_wlan_mode_resp_msg_v01 mode_reply;
	const unsigned pipes = sizeof(target_ce_config_wlan) / sizeof(target_ce_config_wlan[0]);
	const unsigned services = sizeof(target_service_to_ce_map_wlan) / sizeof(target_service_to_ce_map_wlan[0]);
	const unsigned shadows = sizeof(target_shadow_reg_cfg_map) / sizeof(target_shadow_reg_cfg_map[0]);

	bzero(&config, sizeof(config));
	config.tgt_cfg_valid = 1;
	config.tgt_cfg_len = pipes > QMI_WLFW_MAX_NUM_CE_V01 ? QMI_WLFW_MAX_NUM_CE_V01 : pipes;
	for (unsigned i = 0; i < config.tgt_cfg_len; i++) {
		config.tgt_cfg[i].pipe_num = target_ce_config_wlan[i].pipenum;
		config.tgt_cfg[i].pipe_dir = target_ce_config_wlan[i].pipedir;
		config.tgt_cfg[i].nentries = target_ce_config_wlan[i].nentries;
		config.tgt_cfg[i].nbytes_max = target_ce_config_wlan[i].nbytes_max;
		config.tgt_cfg[i].flags = target_ce_config_wlan[i].flags;
	}
	config.svc_cfg_valid = 1;
	config.svc_cfg_len = services > QMI_WLFW_MAX_NUM_SVC_V01 ? QMI_WLFW_MAX_NUM_SVC_V01 : services;
	for (unsigned i = 0; i < config.svc_cfg_len; i++) {
		config.svc_cfg[i].service_id = target_service_to_ce_map_wlan[i].service_id;
		config.svc_cfg[i].pipe_dir = target_service_to_ce_map_wlan[i].pipedir;
		config.svc_cfg[i].pipe_num = target_service_to_ce_map_wlan[i].pipenum;
	}
	config.shadow_reg_valid = 1;
	config.shadow_reg_len = shadows > QMI_WLFW_MAX_NUM_SHADOW_REG_V01 ? QMI_WLFW_MAX_NUM_SHADOW_REG_V01 : shadows;
	for (unsigned i = 0; i < config.shadow_reg_len; i++) {
		config.shadow_reg[i].id = target_shadow_reg_cfg_map[i].ce_id;
		config.shadow_reg[i].offset = target_shadow_reg_cfg_map[i].reg_offset;
	}
	say("wcn3990: 9: the configuration: %u pipes, %u services, %u shadow registers (ath10k's snoc.c)\n", config.tgt_cfg_len,
	    config.svc_cfg_len, config.shadow_reg_len);
	bzero(&config_reply, sizeof(config_reply));
	if (!ask("9: the configuration", QMI_WLFW_WLAN_CFG_REQ_V01, WLFW_WLAN_CFG_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_wlan_cfg_req_msg_v01_ei, &config, wlfw_wlan_cfg_resp_msg_v01_ei, &config_reply, 0, false)) {
		return false;
	}
	bzero(&mode, sizeof(mode));
	bzero(&mode_reply, sizeof(mode_reply));
	mode.mode = QMI_WLFW_MISSION_V01;
	if (!ask("10: the mode: mission", QMI_WLFW_WLAN_MODE_REQ_V01, WLFW_WLAN_MODE_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_wlan_mode_req_msg_v01_ei, &mode, wlfw_wlan_mode_resp_msg_v01_ei, &mode_reply, 0, false)) {
		return false;
	}
	say("wcn3990: 10: THE FIRMWARE IS IN MISSION MODE: the radio waits for its driver at the copy engines\n");
	in_mission = true;
	return true;
}

/*
 * ---- step 11 (wcn3990=3): the copy engines, and the firmware's first HTC message --------------------------------------
 *
 * What ath10k's snoc.c and ce.c do once the mode is "mission" (ath10k_snoc_hif_power_up(), ath10k_snoc_init_pipes(),
 * ath10k_snoc_hif_start(), then ath10k_htc_wait_target()), for three of the twelve engines, by hand:
 *   - the WLAN's DMA stream (dt: iommus <&apps_smmu 0x240 0x1>, qcom,smmu-s1-bypass) let through the SMMU untranslated:
 *     a free match register for 0x240/0x1 with "bypass" behind it, unless the boot chain has one
 *   - the engines' read indices kept in memory of ours (rri on DDR: the wrapper's 0x24c004/8, and bit 19 of every
 *     engine's control register)
 *   - CE0 (host to target, HTC control) a source ring of 32, CE1 (target to host, HTC control and HTT) and CE2 (WMI)
 *     destination rings of 32 and 64 with a buffer of 2048 bytes behind every entry, as host_ce_config_wlan has them
 *   - then CE1's and CE2's completions watched for wcn3990_htc seconds (default 10): the first is to be HTC's "ready"
 *     (message 1: how many credits, of how many bytes, for how many endpoints)
 * The memory is 1 MB at 0x93b00000, inside the compute DSP's reserved memory (cdsp_regions@93900000, never started here;
 * the modem's file buffer is its first 2 MB), seen uncached. Registers: ath10k's hw.c (wcn3990_regs, wcn3990_ce_regs).
 */
#define WLAN_MEMBASE            0x18800000ull   /* dt: reg "membase" of qcom,icnss@18800000, 8 MB */
#define WLAN_MEMBASE_SIZE       0x00800000u
#define CE_BASE(n)              (0x00240000u + 0x1000u * (n))
#define CE_WRAPPER_RRI_LOW      0x0024c004u
#define CE_WRAPPER_RRI_HIGH     0x0024c008u
#define CE_SR_BASE_LO           0x00
#define CE_SR_BASE_HI           0x04
#define CE_SR_SIZE              0x08
#define CE_DR_BASE_LO           0x0c
#define CE_DR_BASE_HI           0x10
#define CE_DR_SIZE              0x14
#define CE_CTRL1                0x18            /* [15:0] dmax, 17 src byte swap, 18 dst byte swap, 19 rri update */
#define CE_HOST_IE              0x2c
#define CE_MISC_IE              0x34
#define CE_MISC_STATUS          0x38
#define CE_SR_WR_INDEX          0x3c
#define CE_DST_WR_INDEX         0x40
#define CE_CURRENT_SRRI         0x44
#define CE_CURRENT_DRRI         0x48
#define CE_SRC_WM               0x4c
#define CE_DST_WM               0x50
#define CE_CTRL1_RRI_UPDATE     (1u << 19)
#define CE_DMA_PHYS             0x93b00000ull
#define CE_DMA_SIZE             0x00100000u
#define CE_BUFFER               2048u
#define SMMU_PHYS               0x15000000ull
#define SMMU_IDR0               0x020
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_S2CR_BYPASS        (1u << 16)
#define SMMU_S2CR_FAULT_DEFAULT 0x200ffu
#define SMMU_IDR1               0x024           /* [7:0] context banks */
#define SMMU_CBAR(n)            (0x1000 + 4 * (n))      /* (in the second page of global registers) */
#define SMMU_CBAR_S1_BYPASS_S2  (1u << 16)      /* CBAR_TYPE_S1_TRANS_S2_BYPASS */
#define SMMU_CB_SCTLR(n)        (0x80000 + 0x1000 * (n))
#define WLAN_STREAM             0x240u
#define WLAN_STREAM_MASK        0x1u

struct ce_desc64 {
	uint64_t addr;
	uint16_t nbytes;
	uint16_t flags;
	uint32_t hash;
};

static volatile uint8_t *wlan, *dma;

static uint32_t
wl_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(wlan + offset);
}

static void
wl_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(wlan + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* The WLAN's stream through the SMMU untranslated: true if it is so already or is made so. */
static bool
smmu_bypass(void)
{
	vm_offset_t smmu = ml_io_map(SMMU_PHYS, 0x2000);
	uint32_t count = *(volatile uint32_t *)(smmu + SMMU_IDR0) & 0xff, free_one = ~0u;
	bool done = false;

	lck_mtx_lock(&hakios_smmu_lock);
	for (uint32_t n = 0; n < count && n < 128; n++) {
		uint32_t smr = *(volatile uint32_t *)(smmu + SMMU_SMR(n)), s2cr = *(volatile uint32_t *)(smmu + SMMU_S2CR(n));
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

		if ((smr & (1u << 31)) && ((id ^ WLAN_STREAM) & ~mask) == 0) {
			say("wcn3990: 0: the SMMU's match register %u (%#x) takes the WLAN's stream already: s2cr %#x\n", n, smr, s2cr);
			done = true;
			break;
		}
		if (free_one == ~0u && !(smr & (1u << 31)) && s2cr == SMMU_S2CR_FAULT_DEFAULT) {
			free_one = n;
		}
	}
	if (!done && free_one != ~0u) {
		/*
		 * Linux's arm-smmu-qcom.c: Qualcomm's hypervisor may not allow "bypass" in a stream-to-context register; it is
		 * written, read back, and if it does not hold, the stream goes instead to a context bank whose translation is off
		 * (the last one, as Linux takes it: CBAR "stage 1 translate, stage 2 bypass", SCTLR 0).
		 */
		uint32_t back;

		say("wcn3990: 0: about to let the WLAN's stream %#x/%#x through the SMMU untranslated: match register %u, s2cr bypass\n",
		    WLAN_STREAM, WLAN_STREAM_MASK, free_one);
		delay_for_interval(1500, NSEC_PER_MSEC);
		*(volatile uint32_t *)(smmu + SMMU_S2CR(free_one)) = SMMU_S2CR_BYPASS;
		__asm__ volatile ("dsb sy" ::: "memory");
		back = *(volatile uint32_t *)(smmu + SMMU_S2CR(free_one));
		say("wcn3990: 0: s2cr %u reads %#x after the write%s\n", free_one, back, (back & (3u << 16)) == SMMU_S2CR_BYPASS ?
		    "" : ": bypass not allowed, a context bank with translation off instead");
		if ((back & (3u << 16)) != SMMU_S2CR_BYPASS) {
			vm_offset_t banks = ml_io_map(SMMU_PHYS, 0x80000 + 0x1000 * 128);
			uint32_t cb = (*(volatile uint32_t *)(smmu + SMMU_IDR1) & 0xff) - 1;

			say("wcn3990: 0: context bank %u: cbar %#x, sctlr %#x\n", cb, *(volatile uint32_t *)(banks + SMMU_CBAR(cb)),
			    *(volatile uint32_t *)(banks + SMMU_CB_SCTLR(cb)));
			if (*(volatile uint32_t *)(banks + SMMU_CB_SCTLR(cb)) & 1) {
				say("wcn3990: 0: it translates (in use): the stream is left as it was\n");
				*(volatile uint32_t *)(smmu + SMMU_S2CR(free_one)) = SMMU_S2CR_FAULT_DEFAULT;
				lck_mtx_unlock(&hakios_smmu_lock);
				return false;
			}
			*(volatile uint32_t *)(banks + SMMU_CBAR(cb)) = SMMU_CBAR_S1_BYPASS_S2;
			*(volatile uint32_t *)(banks + SMMU_CB_SCTLR(cb)) = 0;
			*(volatile uint32_t *)(smmu + SMMU_S2CR(free_one)) = cb;        /* type 0 (translate) to that bank */
			__asm__ volatile ("dsb sy" ::: "memory");
			say("wcn3990: 0: s2cr %u now %#x (context bank %u, translation off)\n", free_one,
			    *(volatile uint32_t *)(smmu + SMMU_S2CR(free_one)), cb);
		}
		*(volatile uint32_t *)(smmu + SMMU_SMR(free_one)) = 1u << 31 | WLAN_STREAM_MASK << 16 | WLAN_STREAM;
		__asm__ volatile ("dsb sy" ::: "memory");
		say("wcn3990: 0: smr %u now %#x\n", free_one, *(volatile uint32_t *)(smmu + SMMU_SMR(free_one)));
		done = true;
	} else if (!done) {
		say("wcn3990: 0: no free match register in the SMMU (%u of them)\n", count);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	return done;
}

static void
ce_source(unsigned ce, uint64_t ring, uint32_t entries, uint32_t dmax)
{
	uint32_t base = CE_BASE(ce);

	wl_wr(base + CE_SR_BASE_LO, (uint32_t)ring);
	wl_wr(base + CE_SR_BASE_HI, (uint32_t)(ring >> 32) & 0x1f);
	wl_wr(base + CE_SR_SIZE, entries);
	wl_wr(base + CE_CTRL1, (wl_rd(base + CE_CTRL1) & ~0xffffu & ~(1u << 17)) | (dmax & 0xffff));
	wl_wr(base + CE_SRC_WM, (wl_rd(base + CE_SRC_WM) & ~0xffffffffu) | (entries & 0xffff));
}

static void
ce_destination(unsigned ce, uint64_t ring, uint32_t entries)
{
	uint32_t base = CE_BASE(ce);

	wl_wr(base + CE_DR_BASE_LO, (uint32_t)ring);
	wl_wr(base + CE_DR_BASE_HI, (wl_rd(base + CE_DR_BASE_HI) & ~0x1fu) | ((uint32_t)(ring >> 32) & 0x1f));
	wl_wr(base + CE_DR_SIZE, entries);
	wl_wr(base + CE_CTRL1, wl_rd(base + CE_CTRL1) & ~(1u << 18));
	wl_wr(base + CE_DST_WM, (wl_rd(base + CE_DST_WM) & ~0xffffffffu) | (entries & 0xffff));
}

/*
 * ---- step 12 (wcn3990=4): HTC, and the WMI service connected (ath10k's htc.c) ----------------------------------------------
 *
 * What the destination rings bring is taken in ring order and the entry given back (its buffer posted again, the write index
 * moved on), as ath10k_snoc_rx_post() keeps them full. CE0 sends: a descriptor (the buffer's address, its length, the
 * endpoint as its transfer id) and the source ring's write index through its shadow register (0x32000 for CE0: the shadow
 * registers were configured with the mode), as ath10k_ce_send_nolock_64() does on the WCN3990.
 */
#define CE_SHADOW_SR_WR_INDEX_CE0 0x00032000u
#define CE_SHADOW_SR_WR_INDEX_CE3 0x0003200cu
#define CE_TX_BUFFERS             0x50000u      /* CE0's: 32 of 2 KB */
#define CE3_RING                  0x4000u       /* CE3's source ring, 32 entries */
#define CE3_TX_BUFFERS            0x60000u      /* CE3's: 32 of 2 KB */

#define RX_RINGS                  7
static struct {
	unsigned ce;
	uint32_t entries, ring_at, buffers_at, read, write;
} rx[RX_RINGS];
static uint32_t tx_write, tx3_write;
static unsigned wmi_endpoint;           /* (WMI's endpoint: defined, 0xff until connected, with its credits below) */
static unsigned htt_endpoint = 0xff;
static volatile uint8_t *shadow;
static uint8_t rx_data[CE_BUFFER];
static unsigned rx_length, rx_ce;

static bool
ce_receive(uint32_t ms)
{
	uint64_t began = now_ms();

	do {
		for (unsigned r = 0; r < RX_RINGS; r++) {
			volatile struct ce_desc64 *ring = (volatile struct ce_desc64 *)(dma + rx[r].ring_at);
			uint32_t e = rx[r].read;

			if (ring[e].nbytes != 0) {
				unsigned length = ring[e].nbytes;
				const volatile uint8_t *buffer = dma + rx[r].buffers_at + e * CE_BUFFER;

				rx_length = length < sizeof(rx_data) ? length : sizeof(rx_data);
				for (unsigned i = 0; i < rx_length; i++) {
					rx_data[i] = buffer[i];
				}
				rx_ce = rx[r].ce;
				ring[e].nbytes = 0;
				rx[r].read = (e + 1) % rx[r].entries;
				rx[r].write = (rx[r].write + 1) % rx[r].entries;
				__asm__ volatile ("dsb sy" ::: "memory");
				wl_wr(CE_BASE(rx[r].ce) + CE_DST_WR_INDEX, rx[r].write);
				rx_length = length;
				return true;
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	} while (now_ms() - began < ms);
	return false;
}

/*
 * One HTC message to an endpoint: the 8 bytes of its header, then the payload. HTC's control (endpoint 0, the service
 * RSVD_CTRL) goes out through CE0, WMI's control through CE3, as snoc.c's map of services to pipes has them
 * (target_service_to_ce_map_wlan: WMI_CONTROL, out, pipe 3).
 */
static bool
htc_send(uint8_t endpoint, const uint8_t *payload, unsigned length)
{
	static uint8_t sequence[8];             /* (per endpoint, as ath10k's ep->seq_no) */
	bool wmi = endpoint != 0;
	volatile struct ce_desc64 *ring = (volatile struct ce_desc64 *)(dma + (wmi ? CE3_RING : 0x1000));
	uint32_t slot = wmi ? tx3_write : tx_write;
	volatile uint8_t *buffer = dma + (wmi ? CE3_TX_BUFFERS : CE_TX_BUFFERS) + slot * CE_BUFFER;
	uint64_t pa = CE_DMA_PHYS + (wmi ? CE3_TX_BUFFERS : CE_TX_BUFFERS) + (uint64_t)slot * CE_BUFFER;

	if (shadow == NULL) {
		shadow = (volatile uint8_t *)ml_io_map(WLAN_MEMBASE + CE_SHADOW_SR_WR_INDEX_CE0, 0x1000);
	}
	if (shadow == NULL || length + 8 > CE_BUFFER) {
		return false;
	}
	buffer[0] = endpoint;
	/* ath10k_htc_prepare_tx_skb(): an endpoint with credit flow (WMI's) asks for its credits back, NEED_CREDIT_UPDATE */
	buffer[1] = endpoint == wmi_endpoint ? 0x01 : 0;
	buffer[2] = (uint8_t)length;
	buffer[3] = (uint8_t)(length >> 8);
	buffer[4] = 0;
	buffer[5] = sequence[endpoint & 7]++;
	buffer[6] = 0;
	buffer[7] = 0;
	for (unsigned i = 0; i < length; i++) {
		buffer[8 + i] = payload[i];
	}
	ring[slot].addr = (pa & 0xffffffffull) | ((pa >> 32) & 0x1f) << 32;
	ring[slot].nbytes = (uint16_t)(length + 8);
	ring[slot].flags = (uint16_t)(endpoint << 4);
	ring[slot].hash = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (wmi) {
		uint64_t asked;

		tx3_write = (slot + 1) % 32;
		*(volatile uint32_t *)(shadow + (CE_SHADOW_SR_WR_INDEX_CE3 - CE_SHADOW_SR_WR_INDEX_CE0)) = tx3_write;
		__asm__ volatile ("dsb sy" ::: "memory");
		/*
		 * (the source read index of CE3, as the engine keeps it in our memory, rri: its low 16 bits; and its register) --
		 * if it does not move within 20 ms, the write index goes to CE3's own register as well, and that is said
		 */
		asked = now_ms();
		while ((*(volatile uint32_t *)(dma + 4 * 3) & 0xffff) != tx3_write && now_ms() - asked < 20) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
		if ((*(volatile uint32_t *)(dma + 4 * 3) & 0xffff) != tx3_write) {
			say("wcn3990: 13:   CE3 did not take it through its shadow register (rri %#x, srri %#x, its write index register %#x): "
			    "written to that register too\n", *(volatile uint32_t *)(dma + 4 * 3), wl_rd(CE_BASE(3) + CE_CURRENT_SRRI),
			    wl_rd(CE_BASE(3) + CE_SR_WR_INDEX));
			wl_wr(CE_BASE(3) + CE_SR_WR_INDEX, tx3_write);
			asked = now_ms();
			while ((*(volatile uint32_t *)(dma + 4 * 3) & 0xffff) != tx3_write && now_ms() - asked < 20) {
				delay_for_interval(1, NSEC_PER_MSEC);
			}
			say("wcn3990: 13:   CE3 then: rri %#x, srri %#x, misc status %#x\n", *(volatile uint32_t *)(dma + 4 * 3),
			    wl_rd(CE_BASE(3) + CE_CURRENT_SRRI), wl_rd(CE_BASE(3) + CE_MISC_STATUS));
		}
	} else {
		tx_write = (slot + 1) % 32;
		*(volatile uint32_t *)shadow = tx_write;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return true;
}

static void show_htc(unsigned ce, const volatile uint8_t *data, unsigned length);
static void htt_rx_poll(void);
static unsigned htt_messages;           /* (HTT's messages from the firmware: counted, the first few said) */

/*
 * HTC's credits for WMI's endpoint: what the firmware gives back comes in the trailer of what it sends (flag 0x2: the
 * trailer's length in byte 4 of the header; its records, id 1 "credits", are pairs of endpoint and count), as
 * ath10k_htc_process_trailer() reads them. A WMI command takes as many credits as its credit size (2184) goes into it.
 */
static unsigned wmi_credits = 2, wmi_credit_size = 2184, wmi_endpoint = 0xff, credit_reports;

static void
htc_credits(const uint8_t *m, unsigned length)
{
	unsigned payload, trailer, at;

	if (length < 8 || !(m[1] & 0x2)) {
		return;
	}
	payload = m[2] | m[3] << 8;
	trailer = m[4];
	if (trailer > payload || 8 + payload > length) {
		return;
	}
	for (at = 8 + payload - trailer; at + 2 <= 8 + payload;) {
		unsigned id = m[at], record = m[at + 1];

		if (at + 2 + record > 8 + payload) {
			break;
		}
		if (id == 1) {
			for (unsigned i = 0; i + 1 < record; i += 2) {
				if (m[at + 2 + i] == wmi_endpoint) {
					wmi_credits += m[at + 3 + i];
					if (credit_reports++ < 6) {
						say("wcn3990: 13:   (the firmware gives %u credits back to endpoint %u: %u now)\n", m[at + 3 + i],
						    wmi_endpoint, wmi_credits);
					}
				}
			}
		}
		at += 2 + record;
	}
}

/* ---- step 13 (wcn3990=5): WMI's init, the firmware's ready, and a scan (ath10k's wmi-tlv.c) ------------------------------- */

#define WMI_INIT_CMD            0x0001
#define WMI_START_SCAN_CMD      0x3001
#define WMI_SCAN_CHAN_LIST_CMD  0x3003
#define WMI_VDEV_CREATE_CMD     0x5001
#define WMI_READY_EVENT         0x0002
#define WMI_SCAN_EVENT          0x3001
#define WMI_MGMT_RX_EVENT       0x7001
#define TAG_ARRAY_UINT32        16
#define TAG_ARRAY_BYTE          17
#define TAG_ARRAY_STRUCT        18
#define TAG_ARRAY_FIXED_STRUCT  19
#define TAG_SERVICE_READY       32
#define TAG_HOST_MEM_REQ        34
#define TAG_READY_EVENT         35
#define TAG_SCAN_EVENT          36
#define TAG_MGMT_RX_HDR         44
#define TAG_INIT_CMD            74
#define TAG_RESOURCE_CONFIG     75
#define TAG_HOST_MEMORY_CHUNK   76
#define TAG_START_SCAN_CMD      77
#define TAG_SCAN_CHAN_LIST_CMD  79
#define TAG_CHANNEL             80
#define TAG_VDEV_CREATE_CMD     86
#define TAG_HAL_REG_CAPABILITIES 33
#define TAG_CHAN_INFO_EVENT     38
#define TAG_PDEV_SET_REGDOMAIN  81
#define WMI_PDEV_SET_REGDOMAIN_CMD 0x4001
#define WMI_CHAN_INFO_EVENT     0x4002
#define WMI_ABI_VER0            0x01000000u
#define WMI_ABI_VER1            53
#define WMI_ABI_NS0             0x5f414351u
#define WMI_ABI_NS1             0x00004c4du
/* (what the firmware asks for at its service ready: after our CE memory. NB: 0x93c00000 is also where the audio DSP's
 * image runs when sound is on; the WCN3990's firmware has asked for nothing so far -- 0 memory requests -- so nothing
 * is written there. If it ever asks, this has to move.) */
#define HOST_MEMORY_PHYS        0x93c00000ull
#define HOST_MEMORY_SIZE        0x01a00000u

static uint8_t wmi_out[CE_BUFFER];
static unsigned wmi_at;

static void
w32(uint32_t value)
{
	wmi_out[wmi_at++] = (uint8_t)value;
	wmi_out[wmi_at++] = (uint8_t)(value >> 8);
	wmi_out[wmi_at++] = (uint8_t)(value >> 16);
	wmi_out[wmi_at++] = (uint8_t)(value >> 24);
}

/* a TLV's header: its length (the value's bytes) and its tag */
static void
wtlv(uint32_t tag, uint32_t length)
{
	w32((length & 0xffff) | tag << 16);
}

static uint32_t
r32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One WMI command: the header (its id), then what has been put into wmi_out after it; credits waited for. */
static bool
wmi_send(unsigned wmi, uint32_t id, const char *what)
{
	unsigned need = (wmi_at + wmi_credit_size - 1) / wmi_credit_size;

	wmi_out[0] = (uint8_t)id;
	wmi_out[1] = (uint8_t)(id >> 8);
	wmi_out[2] = (uint8_t)(id >> 16);
	wmi_out[3] = 0;
	for (unsigned waited = 0; wmi_credits < need && waited < 50; waited++) {
		if (ce_receive(100)) {
			htc_credits(rx_data, rx_length);
		}
	}
	if (wmi_credits < need) {
		say("wcn3990: 13: no credits for %s (%u wanted, %u had)\n", what, need, wmi_credits);
		return false;
	}
	wmi_credits -= need;
	say("wcn3990: 13: WMI: %s (command %#x, %u bytes)\n", what, id, wmi_at);
	return htc_send((uint8_t)wmi, wmi_out, wmi_at);
}

/* A TLV of a message by its tag (at the top level, after the WMI header); NULL if none */
static const uint8_t *
wmi_find(const uint8_t *m, unsigned length, uint32_t tag, unsigned *value_length)
{
	for (unsigned at = 0; at + 4 <= length;) {
		uint32_t header = r32(m + at), size = header & 0xffff;

		if (at + 4 + size > length) {
			return NULL;
		}
		if ((header >> 16) == tag) {
			*value_length = size;
			return m + at + 4;
		}
		at += 4 + size;
	}
	return NULL;
}

/* the next WMI event (its id; its TLVs in *body), credits taken from everything that comes; 0: none within ms */
static uint32_t
wmi_event(unsigned wmi, uint32_t ms, const uint8_t **body, unsigned *length)
{
	uint64_t began = now_ms();

	while (now_ms() - began < ms) {
		if (!ce_receive(ms - (uint32_t)(now_ms() - began))) {
			htt_rx_poll();
			return 0;
		}
		htc_credits(rx_data, rx_length);
		htt_rx_poll();
		if (rx_data[0] == htt_endpoint && htt_messages++ >= 12) {
			continue;
		}
		if (rx_data[0] != wmi || rx_length < 12) {
			char hex[3 * 16 + 1] = "";

			for (unsigned i = 0; i < rx_length && i < 16; i++) {
				snprintf(hex + 3 * i, 4, " %02x", rx_data[i]);
			}
			say("wcn3990: 13:   (CE%u: %u bytes for endpoint %u:%s)\n", rx_ce, rx_length, rx_data[0], hex);
		}
		if (rx_data[0] == wmi && rx_length >= 12) {
			unsigned payload = rx_data[2] | rx_data[3] << 8;
			unsigned trailer = (rx_data[1] & 0x2) ? rx_data[4] : 0;

			*body = rx_data + 12;
			*length = payload > 4 + trailer ? payload - 4 - trailer : 0;
			return r32(rx_data + 8) & 0xffffff;
		}
	}
	return 0;
}

static const struct { uint16_t mhz; uint8_t passive; } channels[] = {
	{ 2412, 0 }, { 2417, 0 }, { 2422, 0 }, { 2427, 0 }, { 2432, 0 }, { 2437, 0 }, { 2442, 0 }, { 2447, 0 }, { 2452, 0 },
	{ 2457, 0 }, { 2462, 0 }, { 2467, 1 }, { 2472, 1 },
	{ 5180, 0 }, { 5200, 0 }, { 5220, 0 }, { 5240, 0 }, { 5260, 1 }, { 5280, 1 }, { 5300, 1 }, { 5320, 1 },
	{ 5500, 1 }, { 5520, 1 }, { 5540, 1 }, { 5560, 1 }, { 5580, 1 }, { 5600, 1 }, { 5620, 1 }, { 5640, 1 }, { 5660, 1 },
	{ 5680, 1 }, { 5700, 1 }, { 5745, 0 }, { 5765, 0 }, { 5785, 0 }, { 5805, 0 }, { 5825, 0 },
};
#define CHANNELS (sizeof(channels) / sizeof(channels[0]))

static struct {
	uint8_t bssid[6];
	char ssid[33];
	uint16_t mhz;
	int snr;
} seen_bss[48];
static unsigned seen_count, chan_infos, scan_events;

static void note_80211(const uint8_t *frame, unsigned frame_length, unsigned mhz, int snr, const char *path);

/* A beacon or probe response from the firmware's management rx event: the network it tells of, noted once. */
static void
note_frame(const uint8_t *body, unsigned length)
{
	unsigned header_length = 0, frame_length = 0;
	const uint8_t *header = wmi_find(body, length, TAG_MGMT_RX_HDR, &header_length);
	const uint8_t *frame = wmi_find(body, length, TAG_ARRAY_BYTE, &frame_length);

	if (header == NULL || frame == NULL || header_length < 24) {
		return;
	}
	note_80211(frame, frame_length, r32(header), (int)r32(header + 4), "wmi");
}

static unsigned scan_mhz;               /* (the channel the scan is on, from its foreign channel events) */

/* An 802.11 frame: if it is a beacon or a probe response, the network it tells of, noted once. */
static void
note_80211(const uint8_t *frame, unsigned frame_length, unsigned mhz, int snr, const char *path)
{
	unsigned subtype, at;
	char ssid[33] = "";
	unsigned i;

	if (frame_length < 36) {
		return;
	}
	subtype = (frame[0] >> 4) & 0xf;
	if ((frame[0] & 0x0c) != 0 || (subtype != 8 && subtype != 5)) {
		return;         /* (management, beacon 8 or probe response 5) */
	}
	for (at = 36; at + 2 <= frame_length; at += 2 + frame[at + 1]) {
		if (frame[at] == 0 && frame[at + 1] <= 32 && at + 2 + frame[at + 1] <= frame_length) {
			memcpy(ssid, frame + at + 2, frame[at + 1]);
			ssid[frame[at + 1]] = 0;
			break;
		}
	}
	for (i = 0; i < seen_count; i++) {
		if (memcmp(seen_bss[i].bssid, frame + 16, 6) == 0) {
			return;
		}
	}
	if (seen_count >= sizeof(seen_bss) / sizeof(seen_bss[0])) {
		return;
	}
	memcpy(seen_bss[seen_count].bssid, frame + 16, 6);
	strlcpy(seen_bss[seen_count].ssid, ssid, sizeof(seen_bss[0].ssid));
	seen_bss[seen_count].mhz = (uint16_t)mhz;
	seen_bss[seen_count].snr = snr;
	say("wcn3990: 14: A NETWORK: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x, %u MHz, snr %d (%s, through %s)\n",
	    ssid[0] ? ssid : "(hidden)", frame[16], frame[17], frame[18], frame[19], frame[20], frame[21], mhz, snr,
	    subtype == 8 ? "beacon" : "probe response", path);
	seen_count++;
}

/*
 * HTT, the data path's own protocol (endpoint 1; out through CE4, in through CE1), set up as ath10k_htt_setup() does it
 * right after WMI's ready: the version asked for, then the receive ring -- ath10k_htt_send_rx_ring_cfg_64(): a ring of
 * 64-bit buffer addresses and the index of the last one given (alloc_idx), which the firmware reads from our memory
 * (ath10k: 2048, 1023 given; here 128, 63 given, below), every frame kind asked for, and where in a buffer each part of the WCN3990's
 * descriptor goes (ath10k_rx_desc_wcn3990_get_offsets(), struct htt_rx_desc_v2: offsets in 4-byte words). Without a ring
 * the radio has nowhere to put what it hears: the scan of 6 November 2026 heard hundreds of thousands of frames and
 * gave nothing up. A buffer is the firmware's done when its attention word has MSDU_DONE (bit 31); it is looked at
 * (a beacon or a probe response in it: a network), cleared, and given back, the ring kept 1023 ahead.
 */
/*
 * In our own CE memory, nowhere else: the first try (6 November 2026) put it in the "host memory" after it, 0x95100000,
 * and that is the audio DSP's running image (linked at 0x93c00000): zeroed under it, the phone hung. A ring of 128
 * (HTT_RX_RING_SIZE_MIN) at 0xb000, its index at 0xc000, 64 buffers of 2 KB at 0xe0000-0x100000: slot i has buffer i % 64,
 * and with at most 63 given at a time no two given slots share one.
 */
#define HTT_RX_RING_ENTRIES     128u
#define HTT_RX_FILL             63u
#define HTT_RX_BUFFERS          64u
#define HTT_RX_BUF              2048u
#define HTT_RX_PHYS             CE_DMA_PHYS
#define HTT_RX_RING_AT          0xb000u
#define HTT_RX_IDX_AT           0xc000u
#define HTT_RX_BUFS_AT          0xe0000u
#define HTT_TX4_BUFFERS         0xd0000u        /* CE4's: 32 of 256 bytes, in our CE memory */
#define CE_SHADOW_SR_WR_INDEX_CE4 0x00032010u

static volatile uint8_t *htt_rx;
static uint32_t htt_rx_read, htt_rx_alloc, htt_rx_done, tx4_write;

static bool
htt_send(const uint8_t *payload, unsigned length, const char *what)
{
	volatile struct ce_desc64 *ring = (volatile struct ce_desc64 *)(dma + 0x5000);
	volatile uint8_t *buffer = dma + HTT_TX4_BUFFERS + tx4_write * 256;
	uint64_t pa = CE_DMA_PHYS + HTT_TX4_BUFFERS + (uint64_t)tx4_write * 256;
	static uint8_t sequence;
	uint64_t asked;

	if (shadow == NULL || htt_endpoint == 0xff || length + 8 > 256) {
		say("wcn3990: 13: HTT: %s not sent (endpoint %u)\n", what, htt_endpoint);
		return false;
	}
	buffer[0] = (uint8_t)htt_endpoint;
	buffer[1] = 0;
	buffer[2] = (uint8_t)length;
	buffer[3] = (uint8_t)(length >> 8);
	buffer[4] = 0;
	buffer[5] = sequence++;
	buffer[6] = 0;
	buffer[7] = 0;
	for (unsigned i = 0; i < length; i++) {
		buffer[8 + i] = payload[i];
	}
	ring[tx4_write].addr = (pa & 0xffffffffull) | ((pa >> 32) & 0x1f) << 32;
	ring[tx4_write].nbytes = (uint16_t)(length + 8);
	ring[tx4_write].flags = (uint16_t)(htt_endpoint << 4);
	ring[tx4_write].hash = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	tx4_write = (tx4_write + 1) % 32;
	*(volatile uint32_t *)(shadow + (CE_SHADOW_SR_WR_INDEX_CE4 - CE_SHADOW_SR_WR_INDEX_CE0)) = tx4_write;
	__asm__ volatile ("dsb sy" ::: "memory");
	asked = now_ms();
	while ((*(volatile uint32_t *)(dma + 4 * 4) & 0xffff) != tx4_write && now_ms() - asked < 20) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if ((*(volatile uint32_t *)(dma + 4 * 4) & 0xffff) != tx4_write) {
		wl_wr(CE_BASE(4) + CE_SR_WR_INDEX, tx4_write);
		asked = now_ms();
		while ((*(volatile uint32_t *)(dma + 4 * 4) & 0xffff) != tx4_write && now_ms() - asked < 20) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
	say("wcn3990: 13: HTT: %s (%u bytes): CE4 rri %#x, srri %#x\n", what, length, *(volatile uint32_t *)(dma + 4 * 4),
	    wl_rd(CE_BASE(4) + CE_CURRENT_SRRI));
	return true;
}

static void
htt_rx_poll(void)
{
	static uint8_t frame[HTT_RX_BUF];
	static unsigned shown;

	if (htt_rx == NULL) {
		return;
	}
	for (unsigned n = 0; n < 64; n++) {
		volatile uint8_t *b = htt_rx + HTT_RX_BUFS_AT + (uint64_t)(htt_rx_read % HTT_RX_BUFFERS) * HTT_RX_BUF;
		uint32_t attention = *(volatile uint32_t *)(b + 1 * 4), msdu = *(volatile uint32_t *)(b + 7 * 4) & 0x3fff;
		unsigned length;

		if (!(attention & 0x80000000u)) {
			return;
		}
		htt_rx_done++;
		/* the frame: the MSDU's payload (word 90) if it holds an 802.11 header, else the 64 bytes of header status (word 74) */
		length = msdu != 0 && msdu <= HTT_RX_BUF - 360 ? msdu : HTT_RX_BUF - 360;
		for (unsigned i = 0; i < length; i++) {
			frame[i] = b[360 + i];
		}
		if ((frame[0] & 0x0c) != 0 || (frame[0] >> 4 != 8 && frame[0] >> 4 != 5)) {
			length = 64;
			for (unsigned i = 0; i < length; i++) {
				frame[i] = b[296 + i];
			}
		}
		if (shown++ < 6) {
			say("wcn3990: 14:   HTT rx buffer %u: attention %#x, msdu %u bytes, header %02x %02x .. %02x:%02x:%02x:%02x:%02x:%02x, "
			    "payload %02x %02x %02x %02x\n", htt_rx_read, attention, msdu, b[296], b[297], b[312], b[313], b[314], b[315],
			    b[316], b[317], b[360], b[361], b[362], b[363]);
		}
		note_80211(frame, length, scan_mhz, 0, "the HTT ring");
		for (unsigned i = 0; i < 364; i += 4) {
			*(volatile uint32_t *)(b + i) = 0;
		}
		htt_rx_read = (htt_rx_read + 1) % HTT_RX_RING_ENTRIES;
		htt_rx_alloc = (htt_rx_alloc + 1) % HTT_RX_RING_ENTRIES;
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint32_t *)(htt_rx + HTT_RX_IDX_AT) = htt_rx_alloc;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void
htt_setup(unsigned wmi)
{
	uint8_t m[40];
	uint64_t began;
	const uint8_t *body;
	unsigned body_length;
	uint32_t level = 3;

	/* wcn3990_htt=N: 0 no HTT set up, 1 the version only, 2 and the receive ring, 3 and the aggregation limits (default) */
	PE_parse_boot_argn("wcn3990_htt", &level, sizeof(level));
	say("wcn3990: 13: HTT: set up to level %u (wcn3990_htt)\n", level);
	if (level == 0) {
		return;
	}
	{
		static const uint8_t version[4] = { 0 };          /* HTT_H2T_MSG_TYPE_VERSION_REQ */

		if (!htt_send(version, sizeof(version), "the version asked for")) {
			return;
		}
	}
	began = now_ms();
	htt_messages = 0;
	while (now_ms() - began < 500 && htt_messages == 0) {
		(void)wmi_event(wmi, 100, &body, &body_length);
	}
	say("wcn3990: 13: HTT: %s\n", htt_messages ? "the firmware answered (its version: the CE1 message above)" :
	    "NO ANSWER to the version request within 500 ms");
	if (level < 2) {
		return;
	}

	for (uint32_t i = 0; i < HTT_RX_RING_ENTRIES; i++) {
		volatile uint8_t *b = dma + HTT_RX_BUFS_AT + (uint64_t)(i % HTT_RX_BUFFERS) * HTT_RX_BUF;

		*(volatile uint64_t *)(dma + HTT_RX_RING_AT + 8 * i) = HTT_RX_PHYS + HTT_RX_BUFS_AT +
		    (uint64_t)(i % HTT_RX_BUFFERS) * HTT_RX_BUF;
		for (unsigned j = 0; j < 364; j += 4) {
			*(volatile uint32_t *)(b + j) = 0;
		}
	}
	htt_rx_read = 0;
	htt_rx_alloc = HTT_RX_FILL;
	*(volatile uint32_t *)(dma + HTT_RX_IDX_AT) = htt_rx_alloc;
	__asm__ volatile ("dsb sy" ::: "memory");
	{
		uint64_t idx = HTT_RX_PHYS + HTT_RX_IDX_AT, base = HTT_RX_PHYS + HTT_RX_RING_AT;
		static const uint16_t offsets[10] = { 74, 90, 27, 37, 4, 26, 7, 12, 1, 2 };
		unsigned at = 0;

		m[at++] = 2;                                    /* HTT_H2T_MSG_TYPE_RX_RING_CFG */
		m[at++] = 1;                                    /* one ring */
		m[at++] = 0; m[at++] = 0;
		for (unsigned i = 0; i < 8; i++) {
			m[at++] = (uint8_t)(idx >> (8 * i));
		}
		for (unsigned i = 0; i < 8; i++) {
			m[at++] = (uint8_t)(base >> (8 * i));
		}
		m[at++] = HTT_RX_RING_ENTRIES & 0xff; m[at++] = HTT_RX_RING_ENTRIES >> 8;
		m[at++] = HTT_RX_BUF & 0xff; m[at++] = HTT_RX_BUF >> 8;
		m[at++] = 0xff; m[at++] = 0xff;                 /* every HTT_RX_RING_FLAGS_, as ath10k sets them */
		m[at++] = HTT_RX_FILL & 0xff; m[at++] = HTT_RX_FILL >> 8;
		for (unsigned i = 0; i < 10; i++) {
			m[at++] = (uint8_t)offsets[i];
			m[at++] = (uint8_t)(offsets[i] >> 8);
		}
		say("wcn3990: 13: HTT: a receive ring of %u at %#llx, %u buffers of %u bytes given, its index at %#llx\n",
		    HTT_RX_RING_ENTRIES, base, HTT_RX_FILL, HTT_RX_BUF, idx);
		(void)htt_send(m, at, "the receive ring");
		htt_rx = dma;           /* (polled from now on) */
	}
	if (level < 3) {
		return;
	}
	{
		static const uint8_t aggregation[4] = { 5, 64, 3, 0 };    /* HTT_H2T_MSG_TYPE_AGGR_CFG: 64 a-mpdu, 3 a-msdu */

		(void)htt_send(aggregation, sizeof(aggregation), "the aggregation limits");
	}
}

static void
wmi_scan(unsigned wmi, const uint8_t *ready, unsigned length)
{
	const uint8_t *event, *requests;
	unsigned event_length = 0, requests_length = 0, body_length = 0, chunks = 0;
	uint8_t mac[6] = { 0 };
	uint64_t next = HOST_MEMORY_PHYS, began;
	const uint8_t *body;
	uint32_t id, seconds = 15;
	struct { uint32_t id, size; uint64_t pa; } chunk[16];

	PE_parse_boot_argn("wcn3990_scan", &seconds, sizeof(seconds));
	/* the service ready: its fixed part (wmi_tlv_svc_rdy_ev: num_mem_reqs at +72), and its memory requests (an array of TLVs) */
	event = wmi_find(ready, length, TAG_SERVICE_READY, &event_length);
	requests = wmi_find(ready, length, TAG_ARRAY_STRUCT, &requests_length);
	if (event == NULL || event_length < 80) {
		say("wcn3990: 13: the service ready has no fixed part this reads\n");
		return;
	}
	say("wcn3990: 13: service ready: firmware %#x, abi %#x %u, rf chains %u, %u memory requests, %u scan channels at most\n",
	    r32(event), r32(event + 4), r32(event + 8), r32(event + 36), r32(event + 72), r32(event + 76));
	for (unsigned at = 0; requests != NULL && at + 4 + 16 <= requests_length && chunks < 16;) {
		uint32_t header = r32(requests + at), size = header & 0xffff;
		const uint8_t *r = requests + at + 4;
		uint32_t units = r32(r + 12), info = r32(r + 8), bytes;

		if ((header >> 16) == TAG_HOST_MEM_REQ && size >= 16) {
			if (info & 0x1) {
				units = 4 + 1;          /* NUM_UNITS_IS_NUM_VDEVS: TARGET_TLV_NUM_VDEVS + 1 */
			} else if (info & 0x6) {
				units = 33 + 1;         /* ... _NUM_PEERS: TARGET_TLV_NUM_PEERS + 1 */
			}
			bytes = (r32(r + 4) * units + 4095) & ~4095u;
			say("wcn3990: 13:   memory request %u: %u units of %u bytes (info %#x): %u bytes at %#llx\n", r32(r), units,
			    r32(r + 4), info, bytes, next);
			if (next + bytes > HOST_MEMORY_PHYS + HOST_MEMORY_SIZE) {
				say("wcn3990: 13:   more than the %u MB there is for it\n", HOST_MEMORY_SIZE >> 20);
				return;
			}
			{
				volatile uint8_t *zero = (volatile uint8_t *)ml_io_map_unmappable(next, bytes, VM_WIMG_WCOMB);

				for (uint32_t i = 0; zero != NULL && i < bytes; i += 8) {
					*(volatile uint64_t *)(zero + i) = 0;
				}
			}
			chunk[chunks].id = r32(r);
			chunk[chunks].size = bytes;
			chunk[chunks].pa = next;
			chunks++;
			next += bytes;
		}
		at += 4 + size;
	}

	/* WMI_INIT: the version, the resource configuration (ath10k_wmi_tlv_op_gen_init()'s values), the memory chunks */
	wmi_at = 4;
	wtlv(TAG_INIT_CMD, 28);
	w32(WMI_ABI_VER0); w32(WMI_ABI_VER1); w32(WMI_ABI_NS0); w32(WMI_ABI_NS1); w32(0); w32(0);
	w32(chunks);
	wtlv(TAG_RESOURCE_CONFIG, 44 * 4);
	{
		static const uint32_t config[44] = {
			4, 33, 0, 0, 2, 66, 32, 0x7, 0x7,               /* vdevs, peers, offload peers, bufs, keys, tids, skid, chains */
			0x64, 0x64, 0x64, 0x28,                          /* rx timeouts */
			1, 4, 4, 4, 8, 0, 0, 0, 0x400, 32, 0, 0, 0, 0, 2,  /* decap (native wifi), scans, bmiss, roam, profiles, mcast, dbg, wds */
			1024 + 32, 2, 1, 0x20, 2, 5, 22, 6, 0, 1, 1, 0, 0, 0, 0, 1u << 9,
		};
		for (unsigned i = 0; i < 44; i++) {
			w32(config[i]);
		}
	}
	wtlv(TAG_ARRAY_STRUCT, chunks * 20);
	for (unsigned i = 0; i < chunks; i++) {
		wtlv(TAG_HOST_MEMORY_CHUNK, 16);
		w32(chunk[i].id); w32((uint32_t)chunk[i].pa); w32(chunk[i].size); w32((uint32_t)(chunk[i].pa >> 32));
	}
	if (!wmi_send(wmi, WMI_INIT_CMD, "init")) {
		return;
	}
	delay_for_interval(50, NSEC_PER_MSEC);
	say("wcn3990: 13:   CE3 after the init: write index %u (ours), its read index %#x (srri), misc status %#x; credits left %u\n",
	    tx3_write, wl_rd(CE_BASE(3) + CE_CURRENT_SRRI), wl_rd(CE_BASE(3) + CE_MISC_STATUS), wmi_credits);
	{
		/* to tell: CE0's indices (it works) beside CE3's, as registers and in our memory; and the SMMU's faults */
		vm_offset_t smmu = ml_io_map(SMMU_PHYS, 0x2000), banks = ml_io_map(SMMU_PHYS + 0x80000 + 0x1000 * 60, 0x1000);

		say("wcn3990: 13:   CE0: write index %u, srri %#x, rri %#x; CE3: rri %#x; CE1: rri %#x; CE2: rri %#x\n", tx_write,
		    wl_rd(CE_BASE(0) + CE_CURRENT_SRRI), *(volatile uint32_t *)(dma + 0), *(volatile uint32_t *)(dma + 12),
		    *(volatile uint32_t *)(dma + 4), *(volatile uint32_t *)(dma + 8));
		say("wcn3990: 13:   SMMU: global fault %#x (syndrome %#x %#x); context bank 60: fault %#x, address %#x%08x, syndrome %#x\n",
		    *(volatile uint32_t *)(smmu + 0x48), *(volatile uint32_t *)(smmu + 0x50), *(volatile uint32_t *)(smmu + 0x54),
		    *(volatile uint32_t *)(banks + 0x58), *(volatile uint32_t *)(banks + 0x64), *(volatile uint32_t *)(banks + 0x60),
		    *(volatile uint32_t *)(banks + 0x68));
	}
	began = now_ms();
	while ((id = wmi_event(wmi, 5000, &body, &body_length)) != 0 && id != WMI_READY_EVENT) {
		say("wcn3990: 13:   (event %#x while waiting for ready)\n", id);
	}
	if (id != WMI_READY_EVENT) {
		say("wcn3990: 13: NO WMI READY within 5 s of the init\n");
		return;
	}
	event = wmi_find(body, body_length, TAG_READY_EVENT, &event_length);
	if (event != NULL && event_length >= 36) {
		memcpy(mac, event + 24, 6);
	}
	say("wcn3990: 13: +%llu ms: THE FIRMWARE IS READY: mac %02x:%02x:%02x:%02x:%02x:%02x, status %u\n", now_ms() - began, mac[0],
	    mac[1], mac[2], mac[3], mac[4], mac[5], event != NULL && event_length >= 36 ? r32(event + 32) : 0);
	if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
		/*
		 * (the WCN3990's firmware has no address of its own: Android gives it one from the phone's own data. Until that is
		 * read here, a locally administered one, the same at every boot: 02:68:6b + the low three bytes of the chip id)
		 */
		mac[0] = 0x02; mac[1] = 0x68; mac[2] = 0x6b; mac[3] = 0x03; mac[4] = 0x02; mac[5] = 0x14;
		say("wcn3990: 13: the firmware gave no address: %02x:%02x:%02x:%02x:%02x:%02x (locally administered) for the vdev\n",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	}

	htt_setup(wmi);

	/*
	 * the regulatory domain, as ath10k_regd_update() gives it after the init: the EEPROM's own (the service ready's HAL
	 * register capabilities), for both bands, the conformance limits ETSI's (0x30, which Turkey's are)
	 */
	{
		unsigned caps_length = 0;
		const uint8_t *caps = wmi_find(ready, length, TAG_HAL_REG_CAPABILITIES, &caps_length);
		uint32_t rd = caps != NULL && caps_length >= 4 ? r32(caps) : 0;

		say("wcn3990: 13: the EEPROM's regulatory domain %#x (%s)\n", rd, caps != NULL ? "from the service ready" : "not given");
		wmi_at = 4;
		wtlv(TAG_PDEV_SET_REGDOMAIN, 24);
		w32(0); w32(rd); w32(rd); w32(rd); w32(0x30); w32(0x30);
		if (!wmi_send(wmi, WMI_PDEV_SET_REGDOMAIN_CMD, "the regulatory domain")) {
			return;
		}
	}
	/* a station's vdev (0), the channels, a scan on it (ath10k_wmi_start_scan_init()'s values) */
	wmi_at = 4;
	wtlv(TAG_VDEV_CREATE_CMD, 20);
	w32(0); w32(2); w32(0);                                 /* vdev 0, WMI_VDEV_TYPE_STA, no subtype */
	w32(mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24); w32(mac[4] | mac[5] << 8);
	if (!wmi_send(wmi, WMI_VDEV_CREATE_CMD, "a station's vdev")) {
		return;
	}
	wmi_at = 4;
	wtlv(TAG_SCAN_CHAN_LIST_CMD, 4);
	w32(CHANNELS);
	wtlv(TAG_ARRAY_STRUCT, CHANNELS * (4 + 24));
	for (unsigned i = 0; i < CHANNELS; i++) {
		uint32_t mode = channels[i].mhz < 3000 ? 1 : 0;         /* MODE_11G, MODE_11A */

		wtlv(TAG_CHANNEL, 24);
		w32(channels[i].mhz); w32(channels[i].mhz); w32(0);
		w32(mode | (channels[i].passive ? 1u << 7 : 0));       /* the mode in the low bits, WMI_CHAN_FLAG_PASSIVE */
		w32(0 | 40u << 8 | 40u << 16);                          /* min, max, reg power (0.5 dBm) */
		w32(0 | 40u << 8);                                      /* antenna gain, max tx power */
	}
	if (!wmi_send(wmi, WMI_SCAN_CHAN_LIST_CMD, "the channels")) {
		return;
	}
	wmi_at = 4;
	wtlv(TAG_START_SCAN_CMD, 15 * 4 + 6 * 4 + 16);
	/*
	 * the scan's and the requester's ids with the host's prefix, 0xA000, as ath10k_wmi_put_start_scan_common() puts them
	 * (WMI_HOST_SCAN_REQ_ID_PREFIX, WMI_HOST_SCAN_REQUESTOR_ID_PREFIX): the firmware sends a scan's events and what it
	 * hears to the host only for those. Without it (6 November 2026) the scan ran over every channel, the radio heard
	 * hundreds of thousands of frames, and nothing of it came up. Scan 0 (ATH10K_SCAN_ID), requester 1, vdev 0, low.
	 */
	w32(0xa000 | 0); w32(0xa000 | 1); w32(0); w32(1);
	w32(0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100);              /* started, completed, bss/foreign channel, dequeued, exit */
	w32(50); w32(150); w32(50); w32(500); w32(0); w32(0); w32(0); w32(20000); w32(5);
	w32((0x4 | 0x8 | 0x10) ^ 0x20);                         /* cck, ofdm rates, channel stats; probe filter inverted (ath10k) */
	w32(0); w32(CHANNELS); w32(1); w32(0); w32(0); w32(3);  /* burst, channels, bssids, ssids, ie, probes */
	w32(mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24); w32(mac[4] | mac[5] << 8);
	w32(0); w32(0);                                         /* (mac mask) */
	wtlv(TAG_ARRAY_UINT32, CHANNELS * 4);
	for (unsigned i = 0; i < CHANNELS; i++) {
		w32(channels[i].mhz);
	}
	wtlv(TAG_ARRAY_FIXED_STRUCT, 0);                        /* no ssids */
	wtlv(TAG_ARRAY_FIXED_STRUCT, 8);                        /* the broadcast bssid */
	w32(0xffffffffu); w32(0xffff);
	wtlv(TAG_ARRAY_BYTE, 0);                                /* no extra ies */
	if (!wmi_send(wmi, WMI_START_SCAN_CMD, "a scan of every channel")) {
		return;
	}
	say("wcn3990: 14: SCANNING for %u s\n", seconds);
	chan_infos = scan_events = 0;
	began = now_ms();
	while (now_ms() - began < seconds * 1000ull) {
		id = wmi_event(wmi, 1000, &body, &body_length);
		if (id == WMI_MGMT_RX_EVENT) {
			note_frame(body, body_length);
		} else if (id == WMI_CHAN_INFO_EVENT) {
			event = wmi_find(body, body_length, TAG_CHAN_INFO_EVENT, &event_length);
			if (event != NULL && event_length >= 24 && chan_infos++ < 80) {
				say("wcn3990: 14:   channel %u MHz: flags %#x, noise %d, rx frames %u\n", r32(event + 4), r32(event + 8),
				    (int)r32(event + 12), event_length >= 36 ? r32(event + 32) : 0);
			}
		} else if (id == WMI_SCAN_EVENT) {
			event = wmi_find(body, body_length, TAG_SCAN_EVENT, &event_length);
			if (event != NULL && event_length >= 8) {
				uint32_t type = r32(event);

				if (type & 0x8 && event_length >= 12) {
					scan_mhz = r32(event + 8);
				}
				if (scan_events++ < 12) {
					say("wcn3990: 14:   scan event %#x, reason %u, %u MHz\n", type, r32(event + 4),
					    event_length >= 12 ? r32(event + 8) : 0);
				}
				if (type & 0x3 || type & 0x40) {
					say("wcn3990: 14: +%llu ms: the scan %s (reason %u)\n", now_ms() - began, type & 0x1 ? "has started" :
					    type & 0x2 ? "HAS COMPLETED" : "FAILED TO START", r32(event + 4));
				}
				if (type & 0x2) {
					break;
				}
			}
		} else if (id != 0) {
			say("wcn3990: 14:   (event %#x)\n", id);
		}
	}
	say("wcn3990: 14: the HTT ring: %u buffers filled by the firmware, %u HTT messages\n", htt_rx_done, htt_messages);
	say("wcn3990: 14: THE SCAN FOUND %u NETWORKS\n", seen_count);
}

/* ath10k_htc_connect_service() for WMI's control, ath10k_htc_start(), and what the firmware says on WMI's endpoint */
static void wmi_scan(unsigned wmi, const uint8_t *service_ready, unsigned length);
static uint8_t service_ready[CE_BUFFER];
static unsigned service_ready_length;

static void
htc_wmi(uint32_t level)
{
	/*
	 * HTC's CONNECT_SERVICE (2) for ATH10K_HTC_SVC_ID_WMI_CONTROL (group 1, index 0: 0x0100), with every transmit credit
	 * HTC's ready gave (2) asked for it, in RECV_ALLOC (flags [15:8]), as ath10k_htc_get_credit_allocation() gives them:
	 * the firmware posts that many buffers on WMI's pipe (CE3). With 0 there, it posted none, and CE3 never moved
	 * (5 November 2026).
	 */
	static const uint8_t connect[8] = { 2, 0, 0x00, 0x01, 0x00, 0x02, 0, 0 };
	/* SETUP_COMPLETE_EX (5): no flags, no bundling */
	static const uint8_t complete[12] = { 5, 0 };
	unsigned wmi = 0xff, events = 0;

	/*
	 * ath10k_core_start()'s order: HTT's data service connected first (ath10k_htt_connect(), ATH10K_HTC_SVC_ID_HTT_DATA_MSG,
	 * group 3 index 0: 0x0300), then WMI's, then HTC started (setup complete)
	 */
	{
		/* (no credits, and credit flow control off: ATH10K_HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL, as for every service but WMI) */
		static const uint8_t connect_htt[8] = { 2, 0, 0x00, 0x03, 0x08, 0x00, 0, 0 };

		say("wcn3990: 12: HTC: connect HTT's data service (0x0300) on endpoint 0 through CE0\n");
		if (htc_send(0, connect_htt, sizeof(connect_htt)) && ce_receive(3000)) {
			show_htc(rx_ce, rx_data, rx_length);
			if (rx_length >= 16 && (rx_data[8] | rx_data[9] << 8) == 3) {
				say("wcn3990: 12: THE SERVICE %#x IS CONNECTED: status %u, endpoint %u, messages of up to %u bytes\n",
				    rx_data[10] | rx_data[11] << 8, rx_data[12], rx_data[13], rx_data[14] | rx_data[15] << 8);
				htt_endpoint = rx_data[13];
			}
		} else {
			say("wcn3990: 12: no answer to HTT's connection\n");
		}
	}
	say("wcn3990: 12: HTC: connect the WMI control service (0x0100) on endpoint 0 through CE0\n");
	if (!htc_send(0, connect, sizeof(connect))) {
		say("wcn3990: 12: CE0 cannot send\n");
		return;
	}
	for (unsigned tries = 0; tries < 8 && wmi == 0xff; tries++) {
		if (!ce_receive(3000)) {
			say("wcn3990: 12: NO ANSWER to the connection within 3 s; CE0 srri %#x, misc status %#x\n",
			    wl_rd(CE_BASE(0) + CE_CURRENT_SRRI), wl_rd(CE_BASE(0) + CE_MISC_STATUS));
			return;
		}
		show_htc(rx_ce, rx_data, rx_length);
		if (rx_length >= 8 + 8 && rx_data[0] == 0 && (rx_data[8] | rx_data[9] << 8) == 3) {
			unsigned service = rx_data[10] | rx_data[11] << 8, status = rx_data[12], max = rx_data[14] | rx_data[15] << 8;

			say("wcn3990: 12: THE SERVICE %#x IS CONNECTED: status %u, endpoint %u, messages of up to %u bytes\n", service,
			    status, rx_data[13], max);
			if (status != 0) {
				return;
			}
			wmi = rx_data[13];
			wmi_endpoint = wmi;
		}
	}
	if (wmi == 0xff) {
		say("wcn3990: 12: no answer to the connection among what came\n");
		return;
	}
	say("wcn3990: 12: HTC: setup complete\n");
	if (!htc_send(0, complete, sizeof(complete))) {
		return;
	}
	/* the firmware's first WMI events: service available (3), service ready (1), then perhaps more (seen: 0x4009, 0x4022) */
	while (events < 6 && ce_receive(events == 0 ? 10000 : 2000)) {
		events++;
		show_htc(rx_ce, rx_data, rx_length);
		if (rx_data[0] == wmi && rx_length >= 12) {
			unsigned id = (rx_data[8] | rx_data[9] << 8 | rx_data[10] << 16 | rx_data[11] << 24) & 0xffffff;

			/* (WMI-TLV's numbers, ath10k's wmi-tlv.h: the start group's events are 1, 2, 3) */
			say("wcn3990: 12: A WMI EVENT on endpoint %u: id %#x%s, %u bytes\n", wmi, id, id == 0x1 ? " (SERVICE READY)" :
			    id == 0x2 ? " (READY)" : id == 0x3 ? " (SERVICE AVAILABLE)" : "", rx_length - 8);
			if (id == 0x1 && rx_length <= sizeof(service_ready)) {
				memcpy(service_ready, rx_data, rx_length);
				service_ready_length = rx_length;
			}
		}
		htc_credits(rx_data, rx_length);
		if (service_ready_length != 0 && events >= 4) {
			break;
		}
	}
	say("wcn3990: 12: %u messages after the setup%s\n", events, events ? "" : ": THE FIRMWARE SAID NOTHING ON WMI");
	if (level >= 5 && service_ready_length != 0) {
		wmi_scan(wmi, service_ready + 8 + 4, service_ready_length - 8 - 4);
	}
}

static void
show_htc(unsigned ce, const volatile uint8_t *data, unsigned length)
{
	char hex[3 * 32 + 1] = "";

	for (unsigned i = 0; i < length && i < 32; i++) {
		snprintf(hex + 3 * i, 4, " %02x", data[i]);
	}
	say("wcn3990: 11:   CE%u: %u bytes:%s\n", ce, length, hex);
	if (length >= 8 + 6) {
		unsigned endpoint = data[0], payload = data[2] | data[3] << 8;
		unsigned message = data[8] | data[9] << 8, credits = data[10] | data[11] << 8, size = data[12] | data[13] << 8;

		say("wcn3990: 11:   HTC: endpoint %u, %u bytes behind the header; message %u%s, %u credits of %u bytes, %u endpoints\n",
		    endpoint, payload, message, message == 1 ? " (READY)" : "", credits, size, length >= 15 ? data[14] : 0);
	}
}

/*
 * ath10k_hw_power_on(): the chip's rails, in the order of ath10k's snoc.c (vdd-0.8-cx-mx, vdd-1.8-xo, vdd-1.3-rfa,
 * vdd-3.3-ch0; and ch1, which surya's tree names too), at the voltages its device tree configures, in high power mode (7,
 * a PMIC5 LDO's): surya's qcom,icnss@18800000 supplies are PM6150 L9 and PM6150L L1, L2, L10 and L11. wcn3990_rails=0
 * leaves them as the boot loader left them.
 */
static void
rails_on(void)
{
	/* (surya's Android, icnss.c: icnss_vreg_info's order, cx-mx at the tree's qcom,vdd-cx-mx-config, the rest at its values) */
	static const struct { const char *rail, *resource; uint32_t millivolts; } rails[] = {
		{ "vdd-cx-mx", "ldoa9", 640 }, { "vdd-1.8-xo", "ldoc1", 1800 }, { "vdd-1.3-rfa", "ldoc2", 1304 },
		{ "vdd-3.3-ch1", "ldoc11", 3312 }, { "vdd-3.3-ch0", "ldoc10", 3312 },
	};
	uint32_t on = 1;

	PE_parse_boot_argn("wcn3990_rails", &on, sizeof(on));
	if (!on) {
		return;
	}
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		int ret = hakios_mpss_regulator(rails[i].resource, rails[i].millivolts, 7, true);

		say("wcn3990: 0: the chip's rail %s (%s, %u mV): %s (%d)\n", rails[i].rail, rails[i].resource, rails[i].millivolts,
		    ret == 0 ? "on" : "NOT ASKED", ret);
		delay_for_interval(2, NSEC_PER_MSEC);
	}
}

static void
copy_engines(uint32_t level)
{
	/*
	 * every destination engine ath10k posts buffers to (ath10k_snoc_rx_post(): CE1, CE2, CE5, CE9, CE10, CE11; and CE8, the
	 * target's to the uMC), 32 entries each here: a firmware with nowhere to put what it sends may hold everything else up
	 */
	static const struct { unsigned ce; uint32_t entries; uint32_t ring_at, buffers_at; } in[RX_RINGS] = {
		{ 1, 32, 0x2000, 0x10000 }, { 2, 64, 0x3000, 0x20000 }, { 5, 32, 0x6000, 0x80000 }, { 8, 32, 0x7000, 0x90000 },
		{ 9, 32, 0x8000, 0xa0000 }, { 10, 32, 0x9000, 0xb0000 }, { 11, 32, 0xa000, 0xc0000 },
	};
	uint32_t seconds = 10, seen = 0;
	uint64_t began;

	uint32_t part = 9;

	PE_parse_boot_argn("wcn3990_htc", &seconds, sizeof(seconds));
	/* wcn3990_ce=N stops after part N: 1 the WLAN's registers read, 2 our DMA memory zeroed, 3 the SMMU, then the rest */
	PE_parse_boot_argn("wcn3990_ce", &part, sizeof(part));
	{
		/* wcn3990_wait=MS: time for the WLAN to come up after the mode before anything of it is touched (default 0) */
		uint32_t wait_ms = 0;

		PE_parse_boot_argn("wcn3990_wait", &wait_ms, sizeof(wait_ms));
		if (wait_ms != 0) {
			say("wcn3990: 11: waiting %u ms after the mode, as wcn3990_wait asks\n", wait_ms);
			delay_for_interval(wait_ms, NSEC_PER_MSEC);
		}
	}
	{
		/* wcn3990_peek=OFS: one word of the WLAN's 8 MB read first, through a page of its own, to tell the region from the
		 * copy engines in it */
		uint32_t peek = ~0u;

		if (PE_parse_boot_argn("wcn3990_peek", &peek, sizeof(peek)) && peek < WLAN_MEMBASE_SIZE) {
			volatile uint8_t *page;

			peek &= ~3u;
			say("wcn3990: 11: about to map the WLAN's page at %#llx\n", WLAN_MEMBASE + (peek & ~0xfffu));
			page = (volatile uint8_t *)ml_io_map(WLAN_MEMBASE + (peek & ~0xfffu), 0x1000);
			say("wcn3990: 11: mapped; about to read one word of it at %#llx (+%#x)\n", WLAN_MEMBASE + peek, peek);
			delay_for_interval(1500, NSEC_PER_MSEC);
			say("wcn3990: 11: it reads %#x\n", *(volatile uint32_t *)(page + (peek & 0xfff)));
		}
	}
	/*
	 * Only the copy engines' 64 KB (CE0 at +0x240000 to the wrapper's +0x24c000) is mapped, not the WLAN's 8 MB: `wlan` is
	 * where +0 would be, and every offset used is in that window.
	 */
	say("wcn3990: 11: about to map the copy engines' registers (64 KB at %#llx)\n", WLAN_MEMBASE + CE_BASE(0));
	{
		volatile uint8_t *window = (volatile uint8_t *)ml_io_map(WLAN_MEMBASE + CE_BASE(0), 0x10000);

		if (window == NULL) {
			say("wcn3990: 11: the WLAN's registers cannot be mapped\n");
			return;
		}
		wlan = window - CE_BASE(0);
	}
	say("wcn3990: 11: mapped\n");
	say("wcn3990: 11: about to read the WLAN's copy engine registers at %#llx (CE1's control and size)\n", WLAN_MEMBASE);
	delay_for_interval(1500, NSEC_PER_MSEC);
	say("wcn3990: 11: the WLAN's registers answer: CE1 ctrl1 %#x, dst size %#x\n", wl_rd(CE_BASE(1) + CE_CTRL1),
	    wl_rd(CE_BASE(1) + CE_DR_SIZE));
	if (part <= 1) {
		return;
	}
	say("wcn3990: 11: about to zero our DMA memory (%#x bytes at %#llx)\n", CE_DMA_SIZE, CE_DMA_PHYS);
	delay_for_interval(1500, NSEC_PER_MSEC);
	dma = (volatile uint8_t *)ml_io_map_unmappable(CE_DMA_PHYS, CE_DMA_SIZE, VM_WIMG_WCOMB);
	if (dma == NULL) {
		say("wcn3990: 11: our DMA memory cannot be mapped\n");
		return;
	}
	for (uint32_t i = 0; i < CE_DMA_SIZE; i += 8) {
		*(volatile uint64_t *)(dma + i) = 0;
	}
	say("wcn3990: 11: our DMA memory is zeroed\n");
	if (part <= 2) {
		return;
	}
	if (part <= 3) {
		return;
	}
	say("wcn3990: 11: the copy engines before: CE0 ctrl1 %#x, CE1 ctrl1 %#x, CE1 dst size %#x, CE1 drri %#x, wrapper rri %#x %#x\n",
	    wl_rd(CE_BASE(0) + CE_CTRL1), wl_rd(CE_BASE(1) + CE_CTRL1), wl_rd(CE_BASE(1) + CE_DR_SIZE),
	    wl_rd(CE_BASE(1) + CE_CURRENT_DRRI), wl_rd(CE_WRAPPER_RRI_LOW), wl_rd(CE_WRAPPER_RRI_HIGH));
	/* ath10k_ce_alloc_rri(): the engines' read indices into our memory (at +0, a word per engine) */
	wl_wr(CE_WRAPPER_RRI_LOW, (uint32_t)CE_DMA_PHYS);
	wl_wr(CE_WRAPPER_RRI_HIGH, (uint32_t)(CE_DMA_PHYS >> 32) & 0x1f);
	for (unsigned ce = 0; ce < 12; ce++) {
		wl_wr(CE_BASE(ce) + CE_CTRL1, wl_rd(CE_BASE(ce) + CE_CTRL1) | CE_CTRL1_RRI_UPDATE);
	}
	/* CE0: the source ring for HTC's control, CE3 the one for WMI's, 32 entries of at most 2048 bytes each */
	ce_source(0, CE_DMA_PHYS + 0x1000, 32, CE_BUFFER);
	ce_source(3, CE_DMA_PHYS + CE3_RING, 32, CE_BUFFER);
	ce_source(4, CE_DMA_PHYS + 0x5000, 32, 256);           /* (HTT's out, nothing sent on it yet) */
	/* CE1, CE2: destination rings, every entry but one given a buffer (ath10k_snoc_rx_post()) */
	for (unsigned r = 0; r < RX_RINGS; r++) {
		volatile struct ce_desc64 *ring = (volatile struct ce_desc64 *)(dma + in[r].ring_at);

		ce_destination(in[r].ce, CE_DMA_PHYS + in[r].ring_at, in[r].entries);
		for (uint32_t e = 0; e < in[r].entries; e++) {
			ring[e].addr = CE_DMA_PHYS + in[r].buffers_at + (uint64_t)e * CE_BUFFER;
			ring[e].nbytes = 0;
			ring[e].flags = 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		wl_wr(CE_BASE(in[r].ce) + CE_DST_WR_INDEX, in[r].entries - 1);
	}
	say("wcn3990: 11: CE0 a source ring of 32, CE1 and CE2 destination rings of 32 and 64 with buffers posted; read indices in "
	    "our memory at %#llx; watching for %u s\n", CE_DMA_PHYS, seconds);
	for (unsigned r = 0; r < RX_RINGS; r++) {
		rx[r].ce = in[r].ce;
		rx[r].entries = in[r].entries;
		rx[r].ring_at = in[r].ring_at;
		rx[r].buffers_at = in[r].buffers_at;
		rx[r].read = 0;
		rx[r].write = in[r].entries - 1;
	}
	tx_write = wl_rd(CE_BASE(0) + CE_SR_WR_INDEX) & 31;
	tx3_write = wl_rd(CE_BASE(3) + CE_SR_WR_INDEX) & 31;
	say("wcn3990: 11: CE3 set up: base %#x %#x, size %u, ctrl1 %#x, watermarks %#x, write index %u, read index %u; CE0 the same: "
	    "base %#x, size %u, ctrl1 %#x, write index %u\n", wl_rd(CE_BASE(3) + CE_SR_BASE_LO), wl_rd(CE_BASE(3) + CE_SR_BASE_HI),
	    wl_rd(CE_BASE(3) + CE_SR_SIZE), wl_rd(CE_BASE(3) + CE_CTRL1), wl_rd(CE_BASE(3) + CE_SRC_WM), tx3_write,
	    wl_rd(CE_BASE(3) + CE_CURRENT_SRRI), wl_rd(CE_BASE(0) + CE_SR_BASE_LO), wl_rd(CE_BASE(0) + CE_SR_SIZE),
	    wl_rd(CE_BASE(0) + CE_CTRL1), tx_write);
	began = now_ms();
	if (!ce_receive(seconds * 1000)) {
		say("wcn3990: 11: NOTHING CAME THROUGH THE COPY ENGINES in %u s; CE1 drri %#x, misc status %#x; CE2 drri %#x, misc "
		    "status %#x\n", seconds, wl_rd(CE_BASE(1) + CE_CURRENT_DRRI), wl_rd(CE_BASE(1) + CE_MISC_STATUS),
		    wl_rd(CE_BASE(2) + CE_CURRENT_DRRI), wl_rd(CE_BASE(2) + CE_MISC_STATUS));
		return;
	}
	say("wcn3990: 11: +%llu ms: THE FIRMWARE HAS SPOKEN THROUGH THE COPY ENGINES\n", now_ms() - began);
	show_htc(rx_ce, rx_data, rx_length);
	(void)seen;
	if (level >= 4) {
		htc_wmi(level);
	}
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
	uint32_t board_id, seconds = 15, level = 1;
	int ret;

	PE_parse_boot_argn("wcn3990", &level, sizeof(level));

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
	/*
	 * 0: the chip's rails, where surya's Android has them up (icnss.c, icnss_driver_event_server_arrive(): power on, then
	 * the indications); with them up only after the mode, the copy engines' registers did not answer (3 November 2026)
	 */
	rails_on();
	/*
	 * ... and the WLAN's DMA stream through the SMMU before the mode can let it move: with the mode "mission" and no match
	 * for stream 0x240 the phone stalled within 3 s, with nothing of the WLAN touched (3 November 2026). Android attaches
	 * it when the driver probes, long before the firmware is spoken to. wcn3990=2 and above.
	 */
	if (level >= 2 && !smmu_bypass()) {
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
		goto out;
	}
	/* 9 and 10 (wcn3990=2): the configuration and the mode, as ath10k sends them when its driver starts */
	if (level >= 2 && mission() && level >= 3) {
		copy_engines(level);
	}
out:
	say("wcn3990: the exchange ends %llu ms after it began: \"MSA ready\" %s, \"FIRMWARE READY\" %s, mission mode %s\n",
	    now_ms() - wlfw.began, wlfw.msa_ready ? "yes" : "no", wlfw.fw_ready ? "YES" : "no", in_mission ? "YES" : "no");
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
