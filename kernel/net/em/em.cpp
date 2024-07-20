/*
 * Copyright (c) 2015, 2016, 2017, 2022, 2023, 2024 Jonas 'Sortie' Termansen.
 * Copyright (c) 2015 Meisaka Yukara.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * net/em/em.cpp
 * 825xx driver.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <timespec.h>

#include <sortix/stat.h>

#include <sortix/kernel/addralloc.h>
#include <sortix/kernel/descriptor.h>
#include <sortix/kernel/inode.h>
#include <sortix/kernel/interrupt.h>
#include <sortix/kernel/ioctx.h>
#include <sortix/kernel/log.h>
#include <sortix/kernel/memorymanagement.h>
#include <sortix/kernel/if.h>
#include <sortix/kernel/packet.h>
#include <sortix/kernel/panic.h>
#include <sortix/kernel/pci.h>
#include <sortix/kernel/pci-mmio.h>
#include <sortix/kernel/random.h>
#include <sortix/kernel/refcount.h>
#include <sortix/kernel/time.h>

#include "../arp.h"
#include "../ether.h"

#include "em.h"
#include "emregs.h"

namespace Sortix {
namespace EM {

static const int RECEIVE_PACKET_COUNT = 32;

static const int FEATURE_EEPROM = 1 << 0; // EEPROM access present
static const int FEATURE_SERDES = 1 << 1; // SerDes/TBI supported
static const int FEATURE_PCIE = 1 << 2; // PCIe Device

enum feature_index
{
	emdefault,
	em8254xS,
	em8254xM,
	em8256xM,
	em8257xM,
	em8257xS,
	em82576S,
	em8258xM,
	em8258xS,
	emfeaturemax
};

static const int feature_table[emfeaturemax] =
{
	[emdefault] = 0,
	[em8254xS] = FEATURE_EEPROM | FEATURE_SERDES,
	[em8254xM] = FEATURE_EEPROM,
	[em8256xM] = FEATURE_PCIE,
	[em8257xM] = FEATURE_PCIE,
	[em8257xS] = FEATURE_SERDES | FEATURE_PCIE,
	[em82576S] = FEATURE_SERDES | FEATURE_PCIE,
	[em8258xM] = FEATURE_PCIE,
	[em8258xS] = FEATURE_SERDES | FEATURE_PCIE,
};

struct device
{
	uint16_t feature_index;
	uint16_t device_id;
};

static const struct device device_table[] =
{
	{ em8254xS, PCI_PRODUCT_INTEL_DH89XXCC_SGMII },
	{ em8254xS, PCI_PRODUCT_INTEL_DH89XXCC_S },
	{ em8254xS, PCI_PRODUCT_INTEL_DH89XXCC_BPLANE },
	{ em8254xS, PCI_PRODUCT_INTEL_DH89XXCC_SFP },
	{ em8254xS, PCI_PRODUCT_INTEL_82542 },
	{ em8254xS, PCI_PRODUCT_INTEL_82543GC_F },
	{ em8254xS, PCI_PRODUCT_INTEL_82543GC_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82544EI_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82544EI_F },
	{ em8254xS, PCI_PRODUCT_INTEL_82544GC_C },
	{ em8254xM, PCI_PRODUCT_INTEL_82544GC_LOM },
	{ em8254xM, PCI_PRODUCT_INTEL_82540EM_D },
	{ em8254xS, PCI_PRODUCT_INTEL_82545EM_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82546EB_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82545EM_F },
	{ em8254xS, PCI_PRODUCT_INTEL_82546EB_F },
	{ em8254xM, PCI_PRODUCT_INTEL_82541EI_C },
	{ em8254xM, PCI_PRODUCT_INTEL_82541ER_LOM },
	{ em8254xM, PCI_PRODUCT_INTEL_82540EM_M },
	{ em8254xM, PCI_PRODUCT_INTEL_82540EP_M },
	{ em8254xM, PCI_PRODUCT_INTEL_82540EP_D },
	{ em8254xM, PCI_PRODUCT_INTEL_82541EI_M },
	{ em8254xM, PCI_PRODUCT_INTEL_82547EI },
	{ em8254xM, PCI_PRODUCT_INTEL_82547EI_M },
	{ em8254xS, PCI_PRODUCT_INTEL_82546EB_CQ },
	{ em8254xS, PCI_PRODUCT_INTEL_82540EP_LP },
	{ em8254xS, PCI_PRODUCT_INTEL_82545GM_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82545GM_F },
	{ em8254xS, PCI_PRODUCT_INTEL_82545GM_S },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IGP_M_AMT },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IGP_AMT },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IGP_C },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IFE },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IGP_M },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_C },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_F },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_S },
	{ em8254xS, PCI_PRODUCT_INTEL_82547GI },
	{ em8254xM, PCI_PRODUCT_INTEL_82541GI_C },
	{ em8254xM, PCI_PRODUCT_INTEL_82541GI_M },
	{ em8254xM, PCI_PRODUCT_INTEL_82541ER_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_C },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_F },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_S },
	{ em8254xM, PCI_PRODUCT_INTEL_82541GI_LF },
	{ em8257xS, PCI_PRODUCT_INTEL_82572EI_C },
	{ em8257xS, PCI_PRODUCT_INTEL_82572EI_F },
	{ em8257xS, PCI_PRODUCT_INTEL_82572EI_S },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_PCIE },
	{ em8257xM, PCI_PRODUCT_INTEL_82573E },
	{ em8257xM, PCI_PRODUCT_INTEL_82573E_IAMT },
	{ em8257xM, PCI_PRODUCT_INTEL_82573E_IDE },
	{ em8257xM, PCI_PRODUCT_INTEL_82573E_KCS },
	{ em8257xM, PCI_PRODUCT_INTEL_82573E_SERIAL },
	{ em8257xS, PCI_PRODUCT_INTEL_80003ES2LAN_CD },
	{ em8257xS, PCI_PRODUCT_INTEL_80003ES2LAN_SD },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_CQ },
	{ em8257xM, PCI_PRODUCT_INTEL_82573L },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_2 },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_AT },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_AF },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_CQ },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_FQ },
	{ em8257xS, PCI_PRODUCT_INTEL_82575EB_C },
	{ em8257xS, PCI_PRODUCT_INTEL_82575EB_S },
	{ em8257xM, PCI_PRODUCT_INTEL_82573L_PL_1 },
	{ em8257xM, PCI_PRODUCT_INTEL_82573V_PM },
	{ em8257xM, PCI_PRODUCT_INTEL_82573E_PM },
	{ em8257xM, PCI_PRODUCT_INTEL_82573L_PL_2 },
	{ em8254xS, PCI_PRODUCT_INTEL_82546GB_CQ_K },
	{ em8257xS, PCI_PRODUCT_INTEL_82572EI },
	{ em8257xS, PCI_PRODUCT_INTEL_80003ES2LAN_C },
	{ em8257xS, PCI_PRODUCT_INTEL_80003ES2LAN_S },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_CQ_LP },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IGP_AMT },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IGP_M },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IFE },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IFE_G },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IFE_GT },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IFE_GT },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_IFE_G },
	{ em82576S, PCI_PRODUCT_INTEL_82576 },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IGP_M_V },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH10_R_BM_LM },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH10_R_BM_LF },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH10_R_BM_V },
	{ em8257xM, PCI_PRODUCT_INTEL_82574L },
	{ em8257xS, PCI_PRODUCT_INTEL_82571PT_CQ },
	{ em8257xS, PCI_PRODUCT_INTEL_82575GB_CQ },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_SD },
	{ em8257xS, PCI_PRODUCT_INTEL_82571EB_SQ },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH10_D_BM_LM },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH10_D_BM_LF },
	{ em8257xS, PCI_PRODUCT_INTEL_82575GB_QP_PM },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_BM },
	{ em82576S, PCI_PRODUCT_INTEL_82576_F },
	{ em82576S, PCI_PRODUCT_INTEL_82576_S },
	{ em82576S, PCI_PRODUCT_INTEL_82576_CQ },
	{ em8257xM, PCI_PRODUCT_INTEL_82577LM },
	{ em8257xM, PCI_PRODUCT_INTEL_82577LC },
	{ em8257xS, PCI_PRODUCT_INTEL_82578DM },
	{ em8257xS, PCI_PRODUCT_INTEL_82578DC },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IGP_M_AMT },
	{ em8257xM, PCI_PRODUCT_INTEL_82574LA },
	{ em8254xS, PCI_PRODUCT_INTEL_82544EI_A4 },
	{ em8254xS, PCI_PRODUCT_INTEL_82544GC_A4 },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH8_82567V_3 },
	{ em8257xM, PCI_PRODUCT_INTEL_82579LM },
	{ em8257xM, PCI_PRODUCT_INTEL_82579V },
	{ em82576S, PCI_PRODUCT_INTEL_82576_NS },
	{ em8258xS, PCI_PRODUCT_INTEL_82583V },
	{ em82576S, PCI_PRODUCT_INTEL_82576_SQ },
	{ em8258xS, PCI_PRODUCT_INTEL_82580_C },
	{ em8258xS, PCI_PRODUCT_INTEL_82580_F },
	{ em8258xS, PCI_PRODUCT_INTEL_82580_S },
	{ em8258xS, PCI_PRODUCT_INTEL_82580_SGMII },
	{ em8258xS, PCI_PRODUCT_INTEL_82580_CD },
	{ em82576S, PCI_PRODUCT_INTEL_82576_NS_S },
	{ em82576S, PCI_PRODUCT_INTEL_I350_C },
	{ em82576S, PCI_PRODUCT_INTEL_I350_F },
	{ em82576S, PCI_PRODUCT_INTEL_I350_S },
	{ em82576S, PCI_PRODUCT_INTEL_I350_SGMII },
	{ em82576S, PCI_PRODUCT_INTEL_82576_CQ_ET2 },
	{ em8258xS, PCI_PRODUCT_INTEL_82580_FQ },
	{ em8257xS, PCI_PRODUCT_INTEL_I210_C },
	{ em8257xS, PCI_PRODUCT_INTEL_I210_F },
	{ em8257xS, PCI_PRODUCT_INTEL_I210_S },
	{ em8257xS, PCI_PRODUCT_INTEL_I210_SGMII },
	{ em8257xS, PCI_PRODUCT_INTEL_I211_C },
	{ em8257xM, PCI_PRODUCT_INTEL_I217_LM },
	{ em8257xM, PCI_PRODUCT_INTEL_I217_V },
	{ em8257xM, PCI_PRODUCT_INTEL_I218_V },
	{ em8257xM, PCI_PRODUCT_INTEL_I218_LM },
	{ em8257xS, PCI_PRODUCT_INTEL_I210_C_NF },
	{ em8257xS, PCI_PRODUCT_INTEL_I210_S_NF },
	{ em8257xM, PCI_PRODUCT_INTEL_I218_LM_2 },
	{ em8257xM, PCI_PRODUCT_INTEL_I218_V_2 },
	{ em8257xM, PCI_PRODUCT_INTEL_I218_LM_3 },
	{ em8257xM, PCI_PRODUCT_INTEL_I218_V_3 },
	{ em8256xM, PCI_PRODUCT_INTEL_ICH9_IGP_C },
	{ em8254xM, PCI_PRODUCT_INTEL_EP80579_LAN_1 },
	{ em8254xM, PCI_PRODUCT_INTEL_EP80579_LAN_4 },
	{ em8254xM, PCI_PRODUCT_INTEL_EP80579_LAN_2 },
	{ em8254xM, PCI_PRODUCT_INTEL_EP80579_LAN_5 },
	{ em8254xM, PCI_PRODUCT_INTEL_EP80579_LAN_3 },
	{ em8254xM, PCI_PRODUCT_INTEL_EP80579_LAN_6 }
};

static const size_t device_table_count =
	sizeof(device_table) / sizeof(device_table[0]);

static const uint32_t understood_interrupts =
	EM_INTERRUPT_TXDW | EM_INTERRUPT_TXQE |
	EM_INTERRUPT_LSC | EM_INTERRUPT_RXDMT0 | EM_INTERRUPT_RXO |
	EM_INTERRUPT_RXT0 | EM_INTERRUPT_MDAC | EM_INTERRUPT_RXCFG |
	EM_INTERRUPT_TXD_LOW | EM_INTERRUPT_SRPD;

struct rx_desc
{
	little_uint64_t address;
	little_uint16_t length;
	little_uint16_t checksum;
	little_uint8_t status;
	little_uint8_t errors;
	little_uint16_t special;
};

struct tx_desc_tcpdata
{
	little_uint64_t address;
	little_uint32_t lencmd;
	little_uint8_t status;
	little_uint8_t opts;
	little_uint16_t special;
};

class EM : public NetworkInterface
{
public:
	EM(uint32_t devaddr, size_t number, int features);
	virtual ~EM();

public:
	virtual bool Send(Ref<Packet> pkt);
	bool Initialize();

private:
	bool Reset();
	__attribute__((format(printf, 2, 3)))
	void Log(const char* format, ...);
	uint32_t Read32(uint32_t reg);
	void Write32(uint32_t reg, uint32_t value);
	bool ReadEEPROM(uint16_t* out, uint16_t reg);
	bool ReadPHY(uint16_t* out, uint8_t reg);
	bool WritePHY(uint8_t reg, uint16_t value);
	bool WaitLinkResolved();
	void RegisterInterrupts();
	bool AddReceiveDescriptor(Ref<Packet> pkt);
	bool AddTransmitDescriptor(Ref<Packet> pkt);
	bool CanAddTransmit();
	static void InterruptHandler(struct interrupt_context*, void*);
	static void InterruptWorkHandler(void* context);
	void OnInterrupt();
	void InterruptWork();

private:
	uint32_t devaddr;
	struct interrupt_handler interrupt_registration;
	struct interrupt_work interrupt_work;
	uint32_t interrupt_work_icr;
	uint8_t interrupt;
	addralloc_t mmio_alloc;
	volatile uint8_t* mmio_base;
	paddrmapped_t rdesc_alloc;
	paddrmapped_t tdesc_alloc;
	int features;
	struct rx_desc* rdesc;
	struct tx_desc_tcpdata* tdesc;
	Ref<Packet>* rpackets;
	Ref<Packet>* tpackets;
	Ref<Packet> tx_queue_first;
	Ref<Packet> tx_queue_last;
	kthread_mutex_t tx_lock;
	kthread_mutex_t eeprom_lock;
	kthread_mutex_t phy_lock;
	uint32_t rx_count;
	uint32_t tx_count;
	uint32_t rx_tail;
	uint32_t rx_prochead;
	uint32_t tx_tail;
	uint32_t tx_prochead;

};

void EM::Log(const char* format, ...)
{
	Log::PrintF("%s: ", ifinfo.name);
	va_list ap;
	va_start(ap, format);
	Log::PrintFV(format, ap);
	va_end(ap);
	Log::PrintF("\n");
}

EM::EM(uint32_t devaddr, size_t number, int features)
{
	snprintf(ifinfo.name, sizeof(ifinfo.name), "em%zu", number);
	ifinfo.type = IF_TYPE_ETHERNET;
	ifinfo.features = IF_FEATURE_ETHERNET_CRC_OFFLOAD;
	ifinfo.addrlen = ETHER_ADDR_LEN;
	ifstatus.mtu = ETHERMTU;
	this->devaddr = devaddr;
	interrupt = 0;
	memset(&interrupt_registration, 0, sizeof(interrupt_registration));
	interrupt_work.handler = InterruptWorkHandler;
	interrupt_work.context = this;
	interrupt_work_icr = 0;
	memset(&mmio_alloc, 0, sizeof(mmio_alloc));
	mmio_base = NULL;
	memset(&rdesc_alloc, 0, sizeof(rdesc_alloc));
	memset(&tdesc_alloc, 0, sizeof(tdesc_alloc));
	this->features = features;
	rdesc = NULL;
	tdesc = NULL;
	rpackets = NULL;
	tpackets = NULL;
	// tx_queue_first has constructor.
	// tx_queue_last has constructor.
	tx_lock = KTHREAD_MUTEX_INITIALIZER;
	eeprom_lock = KTHREAD_MUTEX_INITIALIZER;
	phy_lock = KTHREAD_MUTEX_INITIALIZER;
	rx_count = 0;
	tx_count = 0;
	rx_tail = 0;
	rx_prochead = 0;
	tx_tail = 0;
	tx_prochead = 0;
}

EM::~EM()
{
	assert(false);
}

uint32_t EM::Read32(uint32_t reg)
{
	return *((volatile little_uint32_t*) (mmio_base + reg));
}

void EM::Write32(uint32_t reg, uint32_t value)
{
	*((volatile little_uint32_t*) (mmio_base + reg)) = value;
}

bool EM::ReadEEPROM(uint16_t* out, uint16_t reg)
{
	ScopedLock lock(&eeprom_lock);
	uint32_t value = ((uint32_t) reg) << EM_MAIN_REG_EERD_ADDR_SHIFT |
	                 EM_MAIN_REG_EERD_START;
	Write32(EM_MAIN_REG_EERD, value);
	struct timespec now = Time::Get(CLOCK_MONOTONIC);
	struct timespec end = timespec_add(now, timespec_make(1, 0));
	while ( true )
	{
		now = Time::Get(CLOCK_MONOTONIC);
		if ( (value = Read32(EM_MAIN_REG_EERD)) & EM_MAIN_REG_EERD_DONE )
			break;
		if ( timespec_le(end, now) )
			return false;
	}
	*out = (value & EM_MAIN_REG_EERD_DATA_MASK) >> EM_MAIN_REG_EERD_DATA_SHIFT;
	return true;
}

bool EM::ReadPHY(uint16_t* out, uint8_t reg)
{
	ScopedLock lock(&phy_lock);
	uint32_t mdic = Read32(EM_MAIN_REG_MDIC);
	if ( mdic & EM_MAIN_REG_MDIC_E )
		return false;
	mdic = ((uint32_t) reg) << EM_MAIN_REG_MDIC_REGADD_SHIFT |
	       EM_MAIN_REG_MDIC_PHYADD_PHY_ONE_AND_ONLY |
	       EM_MAIN_REG_MDIC_OP_READ;
	Write32(EM_MAIN_REG_MDIC, mdic);
	uint32_t stop_bits = EM_MAIN_REG_MDIC_R | EM_MAIN_REG_MDIC_E;
	struct timespec now = Time::Get(CLOCK_MONOTONIC);
	struct timespec end = timespec_add(now, timespec_make(1, 0));
	while ( true )
	{
		now = Time::Get(CLOCK_MONOTONIC);
		if ( (mdic = Read32(EM_MAIN_REG_MDIC)) & stop_bits )
			break;
		if ( timespec_le(end, now) )
			return false;
	}
	if ( mdic & EM_MAIN_REG_MDIC_E )
		return false;
	*out = (mdic & EM_MAIN_REG_MDIC_DATA_MASK) >> EM_MAIN_REG_MDIC_DATA_SHIFT;
	return true;
}

bool EM::WritePHY(uint8_t reg, uint16_t value)
{
	ScopedLock lock(&phy_lock);
	uint32_t mdic = Read32(EM_MAIN_REG_MDIC);
	if ( mdic & EM_MAIN_REG_MDIC_E )
		return false;
	mdic = ((uint32_t) value) << EM_MAIN_REG_MDIC_DATA_SHIFT |
           ((uint32_t) reg) << EM_MAIN_REG_MDIC_REGADD_SHIFT |
	       EM_MAIN_REG_MDIC_PHYADD_PHY_ONE_AND_ONLY |
	       EM_MAIN_REG_MDIC_OP_WRITE;
	Write32(EM_MAIN_REG_MDIC, mdic);
	uint32_t stop_bits = EM_MAIN_REG_MDIC_R | EM_MAIN_REG_MDIC_E;
	struct timespec now = Time::Get(CLOCK_MONOTONIC);
	struct timespec end = timespec_add(now, timespec_make(1, 0));
	while ( true )
	{
		now = Time::Get(CLOCK_MONOTONIC);
		if ( (mdic = Read32(EM_MAIN_REG_MDIC)) & stop_bits )
			break;
		if ( timespec_le(end, now) )
			return false;
	}
	if ( mdic & EM_MAIN_REG_MDIC_E )
		return false;
	return true;
}

bool EM::WaitLinkResolved()
{
	uint16_t pstatus;
	uint16_t psstat;
	struct timespec now = Time::Get(CLOCK_MONOTONIC);
	struct timespec end = timespec_add(now, timespec_make(1, 0));
	do
	{
		now = Time::Get(CLOCK_MONOTONIC);
		if ( !ReadPHY(&pstatus, EM_PHY_REG_PSTATUS) ||
		     !ReadPHY(&psstat, EM_PHY_REG_PSSTAT) )
			return Log("error: WaitLinkResolved failed to read PHY"), false;
		if ( !(psstat & EM_PHY_REG_PSSTAT_LINK) )
			return false;
		if ( !(pstatus & EM_PHY_REG_PSTATUS_AN_COMPLETE) )
			continue;
		// TODO: Needed?
		if ( !(psstat & EM_PHY_REG_PSSTAT_SPEED_DUPLEX_RESOLVED) )
			continue;
		return true;
	} while ( timespec_lt(now, end) );
	Log("error: WaitLinkResolved timed out");
	return false;
}

bool EM::AddReceiveDescriptor(Ref<Packet> pkt) // ordered via interrupt worker
{
	uint32_t next_desc = rx_tail + 1;
	if ( rx_count <= next_desc )
		next_desc = 0;
	if ( next_desc == rx_prochead )
		return false;
	struct rx_desc* desc = &rdesc[rx_tail];
	*desc = rx_desc{0, 0, 0, 0, 0, 0};
	desc->status = 0;
	desc->address = pkt->pmap.phys;
	rpackets[rx_tail] = pkt;
	rx_tail = next_desc;
	// TODO: Research whether this is needed, or whether the paging bits do
	//       the right thing. Do those bits work on all systems?
	//asm volatile ("wbinvd");
	Write32(EM_MAIN_REG_RDT, rx_tail);
	return true;
}

bool EM::AddTransmitDescriptor(Ref<Packet> pkt) // tx_lock must be locked.
{
	uint32_t next_desc = tx_tail + 1;
	if ( tx_count <= next_desc )
		next_desc = 0;
	if ( next_desc == tx_prochead )
		return false;
	struct tx_desc_tcpdata* desc = &tdesc[tx_tail];
	desc->address = pkt->pmap.phys;
	desc->lencmd = EM_TDESC_LENGTH(pkt->length) | EM_TDESC_TYPE_TCPDATA |
	               EM_TDESC_CMD_RS | EM_TDESC_CMD_EOP | EM_TDESC_CMD_IFCS;
	desc->status = 0;
	desc->opts = 0;
	desc->special = 0;
	tpackets[tx_tail] = pkt;
	tx_tail = next_desc;
	// TODO: Research whether this is needed, or whether the paging bits do
	//       the right thing. Do those bits work on all systems?
	//asm volatile ("wbinvd");
	Write32(EM_MAIN_REG_TDT, tx_tail);
	return true;
}

bool EM::CanAddTransmit() // tx_lock must be locked.
{
	uint32_t next_desc = tx_tail + 1;
	if ( tx_count <= next_desc )
		next_desc = 0;
	if ( next_desc == tx_prochead )
		return false;
	return true;
}

bool EM::Send(Ref<Packet> pkt)
{
	ScopedLock lock(&tx_lock);
	if ( !tx_queue_first &&
	     CanAddTransmit() &&
	     AddTransmitDescriptor(pkt) )
		return true;
	if ( tx_queue_last )
	{
		tx_queue_last->next = pkt;
		tx_queue_last = pkt;
	}
	else
	{
		tx_queue_first = pkt;
		tx_queue_last = pkt;
	}
	pkt->next.Reset();
	return true;
}

void EM::InterruptHandler(struct interrupt_context*, void* user)
{
	((EM*) user)->OnInterrupt();
}

void EM::RegisterInterrupts()
{
	interrupt = PCI::GetInterruptIndex(devaddr);
	interrupt_registration.handler = EM::InterruptHandler;
	interrupt_registration.context = this;
	Interrupt::RegisterHandler(interrupt, &interrupt_registration);
}

void EM::OnInterrupt()
{
	// TODO: Use "Interrupt Acknowledge Auto Mask Register - IAM (000E0h)" to
	//       automatically mask set interrupts.
	uint32_t icr = Read32(EM_MAIN_REG_ICR);
	if ( !icr )
		return;
	if ( interrupt_work_icr != 0 )
	{
#if 0
		// TODO: This assertion fired! Is it fine to just ignore the interrupt?
		//       Does reading ICR have side effects? Should we read it at the
		//       of the interrupt work handler and reschedule the interrupt
		//       worker?
		if ( interrupt_work_icr )
			PanicF("em: icr=%#x but interrupt_work_icr=%#x\n", icr, interrupt_work_icr);
		assert(interrupt_work_icr == 0);
#endif
		return;
	}
	interrupt_work_icr = icr;
	// Mask further interrupts while processing the interrupt.
	Write32(EM_MAIN_REG_IMC, understood_interrupts);
	Interrupt::ScheduleWork(&interrupt_work);
}

void EM::InterruptWorkHandler(void* context)
{
	((EM*) context)->InterruptWork();
}

void EM::InterruptWork()
{
	uint32_t icr = interrupt_work_icr;
	uint32_t unhandled = icr & 0x1FFFF;
	if ( icr & EM_INTERRUPT_LSC )
	{
		// TODO: This can block the kernel worker thread for a second.
		WaitLinkResolved();
		uint32_t status = Read32(EM_MAIN_REG_STATUS);
		ScopedLock lock(&cfg_lock);
		if ( status & EM_MAIN_REG_STATUS_LU )
			ifstatus.flags |= IF_STATUS_FLAGS_UP;
		else
			ifstatus.flags &= ~IF_STATUS_FLAGS_UP;
		kthread_cond_broadcast(&cfg_cond);
		poll_channel.Signal(PollEventStatus());
		unhandled &= ~EM_INTERRUPT_LSC;
	}
	if ( icr & EM_INTERRUPT_RXDMT0 )
	{
		// Add more receive descriptors when we run out faster than we can
		// process the incoming packets.
		for ( size_t i = 0; i < RECEIVE_PACKET_COUNT; i++ )
		{
			Ref<Packet> buf = GetPacket();
			if ( buf )
			{
				if ( !AddReceiveDescriptor(buf) )
					break;
			}
		}
		unhandled &= ~EM_INTERRUPT_RXDMT0;
	}
	if ( icr & EM_INTERRUPT_MDAC )
	{
		// MDI/O Access Complete
	}
	if ( icr & EM_INTERRUPT_RXT0 )
	{
		// Receive timer expired, check descriptors.
		while ( rx_prochead != rx_tail && rdesc[rx_prochead].status )
		{
			Ref<Packet> rxpacket = rpackets[rx_prochead];
			rpackets[rx_prochead].Reset();
			assert(rxpacket.IsUnique());
			rxpacket->length = rdesc[rx_prochead].length;
			assert(rxpacket->pmap.phys == rdesc[rx_prochead].address);
			rxpacket->netif = this;
			Ether::Handle(rxpacket, true);
			rxpacket.Reset();
			rx_prochead++;
			if ( rx_count <= rx_prochead )
				rx_prochead = 0;
			if ( rx_prochead != rx_tail )
			{
				Ref<Packet> buf = GetPacket();
				// TODO: Design a solution that handles when there's no more
				//       packets available, but later adds packets when they
				//       become available, otherwise the receive queue might
				//       deadlock with no available packets.
				if ( buf )
					AddReceiveDescriptor(buf);
			}
		}
		unhandled &= ~EM_INTERRUPT_RXT0;
	}
	if ( icr & EM_INTERRUPT_RXO )
	{
		// TODO: Receiver overrun, do we need more buffers?
		unhandled &= ~EM_INTERRUPT_RXO;
	}
	kthread_mutex_lock(&tx_lock);
	if ( icr & (EM_INTERRUPT_TXDW | EM_INTERRUPT_TXD_LOW) )
	{
		// Transmit descriptor written back.
		// Transmit descriptor low threshold.
		while ( tx_prochead != tx_tail &&
		        (tdesc[tx_prochead].status & EM_RDESC_STATUS_DD) )
		{
			tpackets[tx_prochead].Reset();
			tx_prochead++;
			if ( tx_count <= tx_prochead )
				tx_prochead = 0;
		}
		unhandled &= ~(EM_INTERRUPT_TXDW | EM_INTERRUPT_TXD_LOW);
	}
	if ( icr & EM_INTERRUPT_TXQE )
	{
		// Transmit queue is empty. Head should equal tail.
		assert(tx_tail < tx_count);
		while ( tx_prochead != tx_tail )
		{
			tpackets[tx_prochead].Reset();
			tx_prochead++;
			if ( tx_count <= tx_prochead )
				tx_prochead = 0;
		}
		unhandled &= ~EM_INTERRUPT_TXQE;
	}
	while ( tx_queue_first && CanAddTransmit() )
	{
		Ref<Packet> pkt = tx_queue_first;
		tx_queue_first = pkt->next;
		pkt->next.Reset();
		AddTransmitDescriptor(pkt);
	}
	kthread_mutex_unlock(&tx_lock);
	interrupt_work_icr = 0;
	// Unmask interrupts so they can be delivered again.
	Write32(EM_MAIN_REG_IMS, understood_interrupts);
}

bool EM::Reset()
{
	uint32_t ctrl;
	uint32_t status;

	PCI::DisableBusMaster(devaddr);
	PCI::DisableInterruptLine(devaddr);

	if ( features & FEATURE_PCIE )
	{
		// For PCIe devices, disable GIO Master prior to reset.
		ctrl = Read32(EM_MAIN_REG_CTRL);
		ctrl |= EM_MAIN_REG_CTRL_GIOMD;
		Write32(EM_MAIN_REG_CTRL, ctrl);
		struct timespec now = Time::Get(CLOCK_MONOTONIC);
		struct timespec end = timespec_add(now, timespec_make(1, 0));
		while ( true )
		{
			now = Time::Get(CLOCK_MONOTONIC);
			status = Read32(EM_MAIN_REG_STATUS);
			if ( !(status & EM_MAIN_REG_STATUS_GIOME) )
				break;
			if ( timespec_le(end, now) )
			{
				Log("error: Failed to disable GIO Master prior to reset");
				return false;
			}
		}
	}

	// Clear all interrupts and disable rx/tx.
	Write32(EM_MAIN_REG_IMC, UINT32_MAX);
	Write32(EM_MAIN_REG_RCTL, 0);
	Write32(EM_MAIN_REG_TCTL, 0);

	// Reset the device, this initializes everything to default settings
	ctrl = Read32(EM_MAIN_REG_CTRL);
	ctrl |= EM_MAIN_REG_CTRL_RST;
	Write32(EM_MAIN_REG_CTRL, ctrl);

	// TODO: The documentation mentioned waiting a short interval here before
	//       checking the register again.

	// Wait for it to finish.
	struct timespec now = Time::Get(CLOCK_MONOTONIC);
	struct timespec end = timespec_add(now, timespec_make(1, 0));
	while ( true )
	{
		// Can exit this wait if card is done loading it's settings.
		if ( Read32(EM_MAIN_REG_EECD) & EM_MAIN_REG_EECD_ARD )
			break;
		// Hack to make sure the control read below is valid.
		// On some hardware, this loop would hang without this.
		// Read all the statisics registers (which we do later anyway).
		for ( uint32_t x = 0; x < 256; x += 4 )
			Read32(EM_STAT_REG_CRCERRS + x);
		// Read, and wait for reset to finish.
		ctrl = Read32(EM_MAIN_REG_CTRL);
		if ( !(ctrl & EM_MAIN_REG_CTRL_PHY_RST) )
			break;
		if ( timespec_le(end, now) )
		{
			Log("error: Timed out waiting for device reset");
			return false;
		}
	}

	// Disable interrupts after the reset.
	Write32(EM_MAIN_REG_IMC, UINT32_MAX);

	if ( features & FEATURE_EEPROM )
	{
		// If we have EEPROM read the MAC directly from it.
		uint16_t macw[3];
		if ( !ReadEEPROM(&macw[0], EM_EEPROM_REG_ETHERNET_ADDR_1) ||
		     !ReadEEPROM(&macw[1], EM_EEPROM_REG_ETHERNET_ADDR_2) ||
		     !ReadEEPROM(&macw[2], EM_EEPROM_REG_ETHERNET_ADDR_3) )
			return Log("error: Failed to read EEPROM"), false;
		ifinfo.addr[0] = macw[0] >> 0 & 0xFF;
		ifinfo.addr[1] = macw[0] >> 8 & 0xFF;
		ifinfo.addr[2] = macw[1] >> 0 & 0xFF;
		ifinfo.addr[3] = macw[1] >> 8 & 0xFF;
		ifinfo.addr[4] = macw[2] >> 0 & 0xFF;
		ifinfo.addr[5] = macw[2] >> 8 & 0xFF;
	}
	else
	{
		uint32_t macd[2];
		// Receive Address[0] is programmed with the hardware mac from PROM or
		// EEPROM after the device is reset.
		macd[0] = Read32(EM_FILTER_REG_RAL);
		macd[1] = Read32(EM_FILTER_REG_RAH);
		ifinfo.addr[0] = macd[0] >> 0  & 0xFF;
		ifinfo.addr[1] = macd[0] >> 8  & 0xFF;
		ifinfo.addr[2] = macd[0] >> 16 & 0xFF;
		ifinfo.addr[3] = macd[0] >> 24 & 0xFF;
		ifinfo.addr[4] = macd[1] >> 0  & 0xFF;
		ifinfo.addr[5] = macd[1] >> 8  & 0xFF;
	}
	memcpy(&cfg.ether.address, ifinfo.addr, sizeof(struct ether_addr));
	Random::Mix(Random::SOURCE_WEAK, ifinfo.addr, sizeof(ifinfo.addr));

	// Enable bus mastering so the card can read/write memory.
	PCI::EnableBusMaster(devaddr);
	PCI::EnableMemoryWrite(devaddr);

	status = Read32(EM_MAIN_REG_STATUS);
	bool inserdes = false;
	if ( features & FEATURE_SERDES )
		inserdes = status & EM_MAIN_REG_STATUS_TBIMODE;

	ctrl = Read32(EM_MAIN_REG_CTRL);

	if ( inserdes )
		ctrl &= ~EM_MAIN_REG_CTRL_LRST; // TBI/SerDes only.
	ctrl |= EM_MAIN_REG_CTRL_ASDE;
	ctrl |= EM_MAIN_REG_CTRL_SLU;
	ctrl &= ~EM_MAIN_REG_CTRL_ILOS;
	ctrl &= ~EM_MAIN_REG_CTRL_FRCSPD;
	ctrl &= ~EM_MAIN_REG_CTRL_FRCDPLX;
	ctrl &= ~EM_MAIN_REG_CTRL_VME;
	// TODO: CTRL.RFCE (hub 8/9/10 pdf 11.4.3.2 says read from phy regs)
	// TODO: CTRL.TFCE (hub 8/9/10 pdf 11.4.3.2 says read from phy regs)
	// TODO: CTRL.ILOS

	Write32(EM_MAIN_REG_CTRL, ctrl);

	// CTRL.FRCSPD = CTRL.FRCDPLX = 0b; CTRL.ASDE = 1b
	// CTRL.FD       Duplex if FRCDPLX is set, ignored otherwise
	// CTRL.SLU      Enable link
	// CTRL.ASDE     Auto-Speed Detection Enable, ignored in TBI/Serdes mode
	// CTRL.RFCE     respond to reception of flow control packets.
	//               Set by Auto-negotiation if negotiation is enabled.
	// CTRL.TFCE     Ethernet controller transmits flow control packets
	//               based on the receive FIFO fullness, or when triggered.
	//               Set by Auto-negotiation if negotiation is enabled.
	// CTRL.ILOS     Invert Loss-of-Signal, reserved on some devices.
	//               set to 0.
	// CTRL.SPEED    Speed if FRCSPD is set, ignored otherwise.
	// CTRL.VME      Enable VLAN Tag removal and processing.

	// STATUS.FD     Reflects the value of CTRL.FD as above.
	// STATUS.LU     Reflects internal link status
	// STATUS.SPEED  Speed status bits reflect speed resolved from ASD function.
	/*
	 For the 82541xx and 82547GI/EI, configure the LED behavior through LEDCTRL.
	 TODO
	*/

	// FCAH and FCAL should contain the flow control Ethernet address.
	// 01:80:C2:00:00:01 and ethertype 0x8808
	Write32(EM_MAIN_REG_FCAH, 0x0100);
	Write32(EM_MAIN_REG_FCAL, 0x00c28001);
	Write32(EM_MAIN_REG_FCT, 0x8808);
	Write32(EM_MAIN_REG_FCTTV, 0);

	// Clear all statistical counters.
	for ( uint32_t x = 0; x < 256; x += 4 )
		Read32(EM_STAT_REG_CRCERRS + x);

	// Setup the descriptor tables,
	Write32(EM_MAIN_REG_RCTL, 0);
	Write32(EM_MAIN_REG_TCTL, 0);
	rx_tail = 0;
	rx_prochead = 0;
	tx_tail = 0;
	tx_prochead = 0;
	rx_count = rdesc_alloc.size / sizeof(struct rx_desc);
	tx_count = tdesc_alloc.size / sizeof(struct tx_desc_tcpdata);
	rdesc = (struct rx_desc*) rdesc_alloc.from;
	tdesc = (struct tx_desc_tcpdata*) tdesc_alloc.from;
	if ( !rpackets )
		rpackets = new Ref<Packet>[rx_count];
	if ( !tpackets )
		tpackets = new Ref<Packet>[tx_count];
	if ( !rpackets || !tpackets )
		return Log("error: Failed to allocate packets: %m"), false;
	Write32(EM_MAIN_REG_RDLEN, rdesc_alloc.size);
	Write32(EM_MAIN_REG_RDH, 0);
	Write32(EM_MAIN_REG_RDT, 0);
	Write32(EM_MAIN_REG_RDBAL, (uint64_t) rdesc_alloc.phys & 0xffffffff);
	Write32(EM_MAIN_REG_RDBAH, (uint64_t) rdesc_alloc.phys >> 32);
	Write32(EM_MAIN_REG_RADV, 0);
	Write32(EM_MAIN_REG_RSRPD, 0);

	Write32(EM_MAIN_REG_TXDCTL,
	        EM_MAIN_REG_TXDCTL_WTHRESH(1) | EM_MAIN_REG_TXDCTL_GRAN);
	uint32_t tipg = Read32(EM_MAIN_REG_TIPG);
	// TODO: Is programming TIPG needed?
	tipg = EM_MAIN_REG_TIPG_IPGT(10) |
	      EM_MAIN_REG_TIPG_IPGR1(4) |
	      EM_MAIN_REG_TIPG_IPGR2(6);
	tipg = EM_MAIN_REG_TIPG_IPGT(8) | /* For 82567 */
	      EM_MAIN_REG_TIPG_IPGR1(8) |
	      EM_MAIN_REG_TIPG_IPGR2(7); // Was 0x902008  on my laptop
	Write32(EM_MAIN_REG_TIPG, tipg);
	Write32(EM_MAIN_REG_TDLEN, tdesc_alloc.size);
	Write32(EM_MAIN_REG_TDH, 0);
	Write32(EM_MAIN_REG_TDT, 0);
	Write32(EM_MAIN_REG_TDBAL, (uint64_t) tdesc_alloc.phys & 0xffffffff);
	Write32(EM_MAIN_REG_TDBAH, (uint64_t) tdesc_alloc.phys >> 32);

	for ( size_t i = 0; i < RECEIVE_PACKET_COUNT; i++ )
	{
		Ref<Packet> buf = GetPacket();
		if ( !buf )
		{
			if ( !i )
				return Log("error: Failed to allocate packets: %m"), false;
			break;
		}
		if ( !AddReceiveDescriptor(buf) )
			break;
	}

	// Enable Receive and Transmit.
	Write32(EM_MAIN_REG_RCTL, EM_MAIN_REG_RCTL_EN | EM_MAIN_REG_RCTL_SBP |
		EM_MAIN_REG_RCTL_MPE | EM_MAIN_REG_RCTL_BAM | EM_MAIN_REG_RCTL_SECRC);
	Write32(EM_MAIN_REG_TCTL, EM_MAIN_REG_TCTL_EN | EM_MAIN_REG_TCTL_PSP |
		EM_MAIN_REG_TCTL_CT(15) | EM_MAIN_REG_TCTL_COLD(64) |
		EM_MAIN_REG_TCTL_RTLC /**/| EM_MAIN_REG_TCTL_RESERVED1);

	// Check if the link is already up since it might not send an interrupt.
	status = Read32(EM_MAIN_REG_STATUS);
	{
		ScopedLock lock(&cfg_lock);
		if ( status & EM_MAIN_REG_STATUS_LU )
			ifstatus.flags |= IF_STATUS_FLAGS_UP;
		else
			ifstatus.flags &= ~IF_STATUS_FLAGS_UP;
		kthread_cond_broadcast(&cfg_cond);
		poll_channel.Signal(PollEventStatus());
	}

	RegisterInterrupts();
	PCI::EnableInterruptLine(devaddr);
	// Reset all the interrupt status (set all interrupts).
	Write32(EM_MAIN_REG_IMS, UINT32_MAX);
	// Disable all interrupts.
	Write32(EM_MAIN_REG_IMC, UINT32_MAX);
	// Enable relevant interrupts.
	Write32(EM_MAIN_REG_IMS, understood_interrupts);

	return true;
}

bool EM::Initialize()
{
	pcibar_t mmio_bar = PCI::GetBAR(devaddr, 0);
	if ( mmio_bar.size() < 128*1024 )
	{
		Log("error: Register area is too small");
		return errno = EINVAL, false;
	}
	if ( !MapPCIBAR(&mmio_alloc, mmio_bar, Memory::PAT_UC) )
	{
		Log("error: Registers could not be mapped: %m");
		return false;
	}
	mmio_base = (volatile uint8_t*) mmio_alloc.from;
	if ( !AllocateAndMapPage(&rdesc_alloc, PAGE_USAGE_DRIVER, Memory::PAT_UC) )
	{
		Log("error: Failed to map descriptor page");
		return false;
	}
	if ( !AllocateAndMapPage(&tdesc_alloc, PAGE_USAGE_DRIVER, Memory::PAT_UC) )
	{
		FreeAllocatedAndMappedPage(&rdesc_alloc);
		Log("error: Failed to map descriptor page");
		return false;
	}
	return Reset();
}

struct Search
{
	const char* devpath;
	Ref<Descriptor> dev;
	size_t number;
};

static bool InitializeDevice(uint32_t devaddr,
                             const pciid_t* /*id*/,
                             const pcitype_t* /*type*/,
                             void* u,
                             void* info)
{
	Search* search = (Search*) u;
	int features = *(const int*) info;
	EM* em = new EM(devaddr, search->number++, features);
	if ( !em )
		return true;
	if ( !em->Initialize() )
		return true;
	if ( !RegisterNetworkInterface(em, search->dev) )
		PanicF("%s: Failed to register as network interface", em->ifinfo.name);
	return true;
}

void Init(const char* devpath, Ref<Descriptor> dev)
{
	Search search = { .devpath = devpath, .dev = dev, .number = 0 };
	pcifind_t* filters = new pcifind_t[device_table_count];
	if ( !filters )
		Panic("em: Failed to allocate PCI filters");
	for ( size_t i = 0; i < device_table_count; i++ )
	{
		const int* features = &feature_table[device_table[i].feature_index];
		uint32_t device_id = device_table[i].device_id;
		filters[i] = pcifind_t((void*) features, PCI_VENDOR_INTEL, device_id,
		                        0x02, 0x00, 0x00);
	}
	PCI::Search(InitializeDevice, &search, filters, device_table_count);
	delete[] filters;
}

} // namespace EM
} // namespace Sortix
