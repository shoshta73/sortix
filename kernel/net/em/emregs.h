/*
 * Copyright (c) 2015, 2016 Jonas 'Sortie' Termansen.
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
 * net/em/emregs.h
 * 825xx registers.
 */

#ifndef SORTIX_NET_EM_EMREGS_H
#define SORTIX_NET_EM_EMREGS_H

#ifndef PCI_VENDOR_INTEL
#define PCI_VENDOR_INTEL                0x8086
#endif

/* device IDs for compatible devices.
 * A large bit of this list (but not all of it)
 * is based on the list found in the OpenBSD em driver. */
#define PCI_PRODUCT_INTEL_DH89XXCC_SGMII	0x0438	/* DH89XXCC SGMII */
#define PCI_PRODUCT_INTEL_DH89XXCC_S		0x043a	/* DH89XXCC SerDes */
#define PCI_PRODUCT_INTEL_DH89XXCC_BPLANE	0x043c	/* DH89XXCC Backplane */
#define PCI_PRODUCT_INTEL_DH89XXCC_SFP		0x0440	/* DH89XXCC SFP */
#define PCI_PRODUCT_INTEL_82542			0x1000	/* 82542 */
#define PCI_PRODUCT_INTEL_82543GC_F		0x1001	/* 82543GC */
#define PCI_PRODUCT_INTEL_82543GC_C		0x1004	/* 82543GC */
#define PCI_PRODUCT_INTEL_82544EI_C		0x1008	/* 82544EI */
#define PCI_PRODUCT_INTEL_82544EI_F		0x1009	/* 82544EI */
#define PCI_PRODUCT_INTEL_82544GC_C		0x100c	/* 82544GC */
#define PCI_PRODUCT_INTEL_82544GC_LOM		0x100d	/* 82544GC */
#define PCI_PRODUCT_INTEL_82540EM_D		0x100e	/* 82540EM Desktop */
#define PCI_PRODUCT_INTEL_82545EM_C		0x100f	/* 82545EM */
#define PCI_PRODUCT_INTEL_82546EB_C		0x1010	/* 82546EB */
#define PCI_PRODUCT_INTEL_82545EM_F		0x1011	/* 82545EM */
#define PCI_PRODUCT_INTEL_82546EB_F		0x1012	/* 82546EB */
#define PCI_PRODUCT_INTEL_82541EI_C		0x1013	/* 82541EI Copper */
#define PCI_PRODUCT_INTEL_82541ER_LOM		0x1014	/* 82541EI */
#define PCI_PRODUCT_INTEL_82540EM_M		0x1015	/* 82540EM Mobile */
#define PCI_PRODUCT_INTEL_82540EP_M		0x1016	/* 82540EP Mobile */
#define PCI_PRODUCT_INTEL_82540EP_D		0x1017	/* 82540EP Desktop */
#define PCI_PRODUCT_INTEL_82541EI_M		0x1018	/* 82541EI Mobile */
#define PCI_PRODUCT_INTEL_82547EI		0x1019	/* 82547EI */
#define PCI_PRODUCT_INTEL_82547EI_M		0x101a	/* 82547EI Mobile */
#define PCI_PRODUCT_INTEL_82546EB_CQ		0x101d	/* 82546EB */
#define PCI_PRODUCT_INTEL_82540EP_LP		0x101e	/* 82540EP */
#define PCI_PRODUCT_INTEL_82545GM_C		0x1026	/* 82545GM */
#define PCI_PRODUCT_INTEL_82545GM_F		0x1027	/* 82545GM */
#define PCI_PRODUCT_INTEL_82545GM_S		0x1028	/* 82545GM */
#define PCI_PRODUCT_INTEL_ICH8_IGP_M_AMT	0x1049	/* ICH8 IGP M AMT */
#define PCI_PRODUCT_INTEL_ICH8_IGP_AMT		0x104a	/* ICH8 IGP AMT */
#define PCI_PRODUCT_INTEL_ICH8_IGP_C		0x104b	/* ICH8 IGP C */
#define PCI_PRODUCT_INTEL_ICH8_IFE		0x104c	/* ICH8 IFE */
#define PCI_PRODUCT_INTEL_ICH8_IGP_M		0x104d	/* ICH8 IGP M */
#define PCI_PRODUCT_INTEL_82571EB_C		0x105e	/* 82571EB */
#define PCI_PRODUCT_INTEL_82571EB_F		0x105f	/* 82571EB */
#define PCI_PRODUCT_INTEL_82571EB_S		0x1060	/* 82571EB */
#define PCI_PRODUCT_INTEL_82547GI		0x1075	/* 82547GI */
#define PCI_PRODUCT_INTEL_82541GI_C		0x1076	/* 82541GI Copper */
#define PCI_PRODUCT_INTEL_82541GI_M		0x1077	/* 82541GI Mobile */
#define PCI_PRODUCT_INTEL_82541ER_C		0x1078	/* 82541ER Copper */
#define PCI_PRODUCT_INTEL_82546GB_C		0x1079	/* 82546GB */
#define PCI_PRODUCT_INTEL_82546GB_F		0x107a	/* 82546GB */
#define PCI_PRODUCT_INTEL_82546GB_S		0x107b	/* 82546GB */
#define PCI_PRODUCT_INTEL_82541GI_LF		0x107c	/* 82541GI */
#define PCI_PRODUCT_INTEL_82572EI_C		0x107d	/* 82572EI */
#define PCI_PRODUCT_INTEL_82572EI_F		0x107e	/* 82572EI */
#define PCI_PRODUCT_INTEL_82572EI_S		0x107f	/* 82572EI */
#define PCI_PRODUCT_INTEL_82546GB_PCIE		0x108a	/* 82546GB */
#define PCI_PRODUCT_INTEL_82573E		0x108b	/* 82573E */
#define PCI_PRODUCT_INTEL_82573E_IAMT		0x108c	/* 82573E */
#define PCI_PRODUCT_INTEL_82573E_IDE		0x108d	/* 82573E IDE */
#define PCI_PRODUCT_INTEL_82573E_KCS		0x108e	/* 82573E KCS */
#define PCI_PRODUCT_INTEL_82573E_SERIAL		0x108f	/* 82573E Serial */
#define PCI_PRODUCT_INTEL_80003ES2LAN_CD	0x1096	/* 80003ES2 */
#define PCI_PRODUCT_INTEL_80003ES2LAN_SD	0x1098	/* 80003ES2 */
#define PCI_PRODUCT_INTEL_82546GB_CQ		0x1099	/* 82546GB */
#define PCI_PRODUCT_INTEL_82573L		0x109a	/* 82573L */
#define PCI_PRODUCT_INTEL_82546GB_2		0x109b	/* 82546GB */
#define PCI_PRODUCT_INTEL_82571EB_AT		0x10a0	/* 82571EB */
#define PCI_PRODUCT_INTEL_82571EB_AF		0x10a1	/* 82571EB */
#define PCI_PRODUCT_INTEL_82571EB_CQ		0x10a4	/* 82571EB */
#define PCI_PRODUCT_INTEL_82571EB_FQ		0x10a5	/* 82571EB */
#define PCI_PRODUCT_INTEL_82575EB_C		0x10a7	/* 82575EB */
#define PCI_PRODUCT_INTEL_82575EB_S		0x10a9	/* 82575EB */
#define PCI_PRODUCT_INTEL_82573L_PL_1		0x10b0	/* 82573L */
#define PCI_PRODUCT_INTEL_82573V_PM		0x10b2	/* 82573V */
#define PCI_PRODUCT_INTEL_82573E_PM		0x10b3	/* 82573E */
#define PCI_PRODUCT_INTEL_82573L_PL_2		0x10b4	/* 82573L */
#define PCI_PRODUCT_INTEL_82546GB_CQ_K		0x10b5	/* 82546GB */
#define PCI_PRODUCT_INTEL_82572EI		0x10b9	/* 82572EI */
#define PCI_PRODUCT_INTEL_80003ES2LAN_C		0x10ba	/* 80003ES2 */
#define PCI_PRODUCT_INTEL_80003ES2LAN_S		0x10bb	/* 80003ES2 */
#define PCI_PRODUCT_INTEL_82571EB_CQ_LP		0x10bc	/* 82571EB */
#define PCI_PRODUCT_INTEL_ICH9_IGP_AMT		0x10bd	/* ICH9 IGP AMT */
#define PCI_PRODUCT_INTEL_ICH9_IGP_M		0x10bf	/* ICH9 IGP M */
#define PCI_PRODUCT_INTEL_ICH9_IFE		0x10c0	/* ICH9 IFE */
#define PCI_PRODUCT_INTEL_ICH9_IFE_G		0x10c2	/* ICH9 IFE G */
#define PCI_PRODUCT_INTEL_ICH9_IFE_GT		0x10c3	/* ICH9 IFE GT */
#define PCI_PRODUCT_INTEL_ICH8_IFE_GT		0x10c4	/* ICH8 IFE GT */
#define PCI_PRODUCT_INTEL_ICH8_IFE_G		0x10c5	/* ICH8 IFE G */
#define PCI_PRODUCT_INTEL_82576			0x10c9	/* 82576 */
#define PCI_PRODUCT_INTEL_ICH9_IGP_M_V		0x10cb	/* ICH9 IGP M V */
#define PCI_PRODUCT_INTEL_ICH10_R_BM_LM		0x10cc	/* ICH10 R BM LM */
#define PCI_PRODUCT_INTEL_ICH10_R_BM_LF		0x10cd	/* ICH10 R BM LF */
#define PCI_PRODUCT_INTEL_ICH10_R_BM_V		0x10ce	/* ICH10 R BM V */
#define PCI_PRODUCT_INTEL_82574L		0x10d3	/* 82574L */
#define PCI_PRODUCT_INTEL_82571PT_CQ		0x10d5	/* 82571PT */
#define PCI_PRODUCT_INTEL_82575GB_CQ		0x10d6	/* 82575GB */
#define PCI_PRODUCT_INTEL_82571EB_SD		0x10d9	/* 82571EB */
#define PCI_PRODUCT_INTEL_82571EB_SQ		0x10da	/* 82571EB */
#define PCI_PRODUCT_INTEL_ICH10_D_BM_LM		0x10de	/* ICH10 D BM LM */
#define PCI_PRODUCT_INTEL_ICH10_D_BM_LF		0x10df	/* ICH10 D BM LF */
#define PCI_PRODUCT_INTEL_82575GB_QP_PM		0x10e2	/* 82575GB */
#define PCI_PRODUCT_INTEL_ICH9_BM		0x10e5	/* ICH9 BM */
#define PCI_PRODUCT_INTEL_82576_F		0x10e6	/* 82576 */
#define PCI_PRODUCT_INTEL_82576_S		0x10e7	/* 82576 */
#define PCI_PRODUCT_INTEL_82576_CQ		0x10e8	/* 82576 */
#define PCI_PRODUCT_INTEL_82577LM		0x10ea	/* 82577LM */
#define PCI_PRODUCT_INTEL_82577LC		0x10eb	/* 82577LC */
#define PCI_PRODUCT_INTEL_82578DM		0x10ef	/* 82578DM */
#define PCI_PRODUCT_INTEL_82578DC		0x10f0	/* 82578DC */
#define PCI_PRODUCT_INTEL_ICH9_IGP_M_AMT	0x10f5	/* ICH9 IGP M AMT */
#define PCI_PRODUCT_INTEL_82574LA		0x10f6	/* 82574L */
#define PCI_PRODUCT_INTEL_82544EI_A4		0x1107	/* 82544EI-A4 */
#define PCI_PRODUCT_INTEL_82544GC_A4		0x1112	/* 82544EI-A4 */
#define PCI_PRODUCT_INTEL_ICH8_82567V_3		0x1501	/* ICH8 82567V-3 */
#define PCI_PRODUCT_INTEL_82579LM		0x1502	/* 82579LM */
#define PCI_PRODUCT_INTEL_82579V		0x1503	/* 82579V */
#define PCI_PRODUCT_INTEL_82576_NS		0x150a	/* 82576NS */
#define PCI_PRODUCT_INTEL_82583V		0x150c	/* 82583V */
#define PCI_PRODUCT_INTEL_82576_SQ		0x150d	/* 82576 SerDes QP */
#define PCI_PRODUCT_INTEL_82580_C		0x150e	/* 82580 */
#define PCI_PRODUCT_INTEL_82580_F		0x150f	/* 82580 */
#define PCI_PRODUCT_INTEL_82580_S		0x1510	/* 82580 */
#define PCI_PRODUCT_INTEL_82580_SGMII		0x1511	/* 82580 */
#define PCI_PRODUCT_INTEL_82580_CD		0x1516	/* 82580 */
#define PCI_PRODUCT_INTEL_82576_NS_S		0x1518	/* 82576NS */
#define PCI_PRODUCT_INTEL_I350_C		0x1521	/* I350 */
#define PCI_PRODUCT_INTEL_I350_F		0x1522	/* I350 Fiber */
#define PCI_PRODUCT_INTEL_I350_S		0x1523	/* I350 SerDes */
#define PCI_PRODUCT_INTEL_I350_SGMII		0x1524	/* I350 SGMII */
#define PCI_PRODUCT_INTEL_82576_CQ_ET2		0x1526	/* 82576 */
#define PCI_PRODUCT_INTEL_82580_FQ		0x1527	/* 82580 QF */
#define PCI_PRODUCT_INTEL_I210_C		0x1533	/* I210 */
#define PCI_PRODUCT_INTEL_I210_F		0x1536	/* I210 Fiber */
#define PCI_PRODUCT_INTEL_I210_S		0x1537	/* I210 SerDes */
#define PCI_PRODUCT_INTEL_I210_SGMII		0x1538	/* I210 SGMII */
#define PCI_PRODUCT_INTEL_I211_C		0x1539	/* I211 */
#define PCI_PRODUCT_INTEL_I217_LM		0x153a	/* I217-LM */
#define PCI_PRODUCT_INTEL_I217_V		0x153b	/* I217-V */
#define PCI_PRODUCT_INTEL_I218_V		0x1559	/* I218-V */
#define PCI_PRODUCT_INTEL_I218_LM		0x155a	/* I218-LM */
#define PCI_PRODUCT_INTEL_I210_C_NF		0x157b	/* I210 */
#define PCI_PRODUCT_INTEL_I210_S_NF		0x157c	/* I210 SerDes */
#define PCI_PRODUCT_INTEL_I218_LM_2		0x15a0	/* I218-LM */
#define PCI_PRODUCT_INTEL_I218_V_2		0x15a1	/* I218-V */
#define PCI_PRODUCT_INTEL_I218_LM_3		0x15a2	/* I218-LM */
#define PCI_PRODUCT_INTEL_I218_V_3		0x15a3	/* I218-V */
#define PCI_PRODUCT_INTEL_ICH9_IGP_C		0x294c	/* ICH9 IGP C */
#define PCI_PRODUCT_INTEL_EP80579_LAN_1		0x5040	/* EP80579 LAN */
#define PCI_PRODUCT_INTEL_EP80579_LAN_4		0x5041	/* EP80579 LAN */
#define PCI_PRODUCT_INTEL_EP80579_LAN_2		0x5044	/* EP80579 LAN */
#define PCI_PRODUCT_INTEL_EP80579_LAN_5		0x5045	/* EP80579 LAN */
#define PCI_PRODUCT_INTEL_EP80579_LAN_3		0x5048	/* EP80579 LAN */
#define PCI_PRODUCT_INTEL_EP80579_LAN_6		0x5049	/* EP80579 LAN */

/* Device Control */
#define EM_MAIN_REG_CTRL                0x0000
/* Full-Duplex */
#define EM_MAIN_REG_CTRL_FD                       (1U << 0)
/* GIO Master Disable (PCIe chipsets only) */
#define EM_MAIN_REG_CTRL_GIOMD                    (1U << 2)
/* Link Reset (not applicable to the 82540EP/EM, 82541xx, or 82547GI/EI) */
#define EM_MAIN_REG_CTRL_LRST                     (1U << 3)
/* Auto-Speed Detection Enable */
#define EM_MAIN_REG_CTRL_ASDE                     (1U << 5)
/* Set Link Up */
#define EM_MAIN_REG_CTRL_SLU                      (1U << 6)
/* Invert Loss-of-Signal (LOS) */
#define EM_MAIN_REG_CTRL_ILOS                     (1U << 7)
/* Invert Loss-of-Signal (LOS) */
#define EM_MAIN_REG_CTRL_SPEED_MASK               (3U << 8)
#define EM_MAIN_REG_CTRL_SPEED_10MBS              (0U << 8)
#define EM_MAIN_REG_CTRL_SPEED_100MBS             (1U << 8)
#define EM_MAIN_REG_CTRL_SPEED_1000MBS            (2U << 8)
/* Force Speed */
#define EM_MAIN_REG_CTRL_FRCSPD                   (1U << 11)
/* Force Duplex */
#define EM_MAIN_REG_CTRL_FRCDPLX                  (1U << 12)
/* SDP0 Data Value */
#define EM_MAIN_REG_CTRL_SDP0_DATA                (1U << 18)
/* SDP1 Data Value */
#define EM_MAIN_REG_CTRL_SDP1_DATA                (1U << 19)
/* D3Cold Wakeup Capability Advertisement Enable (not applicable to the 82541ER) */
#define EM_MAIN_REG_CTRL_ADVD3WUC                 (1U << 20)
/* PHY Power-Management Enable */
#define EM_MAIN_REG_CTRL_EN_PHY_PWR_MGMT          (1U << 21)
/* SDP0 Pin Directionality */
#define EM_MAIN_REG_CTRL_SDP0_IODIR               (1U << 22)
/* SDP1 Pin Directionality */
#define EM_MAIN_REG_CTRL_SDP1_IODIR               (1U << 23)
/* Device Reset */
#define EM_MAIN_REG_CTRL_RST                      (1U << 26)
/* Receive Flow Control Enable */
#define EM_MAIN_REG_CTRL_RFCE                     (1U << 27)
/* Transmit Flow Control Enable */
#define EM_MAIN_REG_CTRL_TFCE                     (1U << 28)
/* VLAN Mode Enable (not applicable to the 82541ER) */
#define EM_MAIN_REG_CTRL_VME                      (1U << 30)
/* PHY Reset */
#define EM_MAIN_REG_CTRL_PHY_RST                  (1U << 31)

/* Device Status */
#define EM_MAIN_REG_STATUS              0x0008

/* Full Duplex */
#define EM_MAIN_REG_STATUS_FD                     (1U << 0)
/* Link Up */
#define EM_MAIN_REG_STATUS_LU                     (1U << 1)
/* LAN ID, LAN A or LAN B */
#define EM_MAIN_REG_STATUS_LANID_MASK             (3U << 2)
#define EM_MAIN_REG_STATUS_LANID_LANA             (0U << 2)
#define EM_MAIN_REG_STATUS_LANID_LANB             (1U << 2)
/* TBI/SerDes mode - reserved bit on some devices */
#define EM_MAIN_REG_STATUS_TBIMODE                (1U << 5)
/* Link Speed */
#define EM_MAIN_REG_STATUS_SPEED_MASK             (3U << 6)
#define EM_MAIN_REG_STATUS_SPEED_10MBS            (0U << 6)
#define EM_MAIN_REG_STATUS_SPEED_100MBS           (1U << 6)
/* There are two definitions for 1000Mb/s */
#define EM_MAIN_REG_STATUS_SPEED_1000MBS          (2U << 6)
#define EM_MAIN_REG_STATUS_SPEED_1000MBSA         (3U << 6)

#define EM_MAIN_REG_STATUS_GIOME                  (1U << 19)

/* EEPROM/Flash Control/Data */
#define EM_MAIN_REG_EECD                0x0010
#define EM_MAIN_REG_EECD_ARD                      (1U << 9)

/* EEPROM Read (not applicable to the 82544GC/EI) */
#define EM_MAIN_REG_EERD                0x0014
#define EM_MAIN_REG_EERD_START                    (1U << 0)
#define EM_MAIN_REG_EERD_DONE                     (1U << 4)
#define EM_MAIN_REG_EERD_ADDR_SHIFT               8
#define EM_MAIN_REG_EERD_ADDR_MASK                (0xFFU << 8)
#define EM_MAIN_REG_EERD_DATA_SHIFT               16
#define EM_MAIN_REG_EERD_DATA_MASK                (0xFFFFU << 16)

/* Flash Access (applicable to the 82541xx and 82547GI/EI only) */
#define EM_MAIN_REG_FLA                 0x001C

/* Extended Device Control */
#define EM_MAIN_REG_CTRL_EXT            0x0018
/* General Purpose Interrupt Enables */
#define EM_MAIN_REG_CTRL_EXT_GPI_EN_MASK          (2U << 3)
#define EM_MAIN_REG_CTRL_EXT_GPI_EN_SHIFT         3
/* PHY Interrupt Value */
#define EM_MAIN_REG_CTRL_EXT_PHYINT               (1U << 5)
/* SDP6[2] Data Value */
#define EM_MAIN_REG_CTRL_EXT_SDP2_DATA            (1U << 6)
#define EM_MAIN_REG_CTRL_EXT_SDP6_DATA            (1U << 6)
/* SDP7[3] Data Value */
#define EM_MAIN_REG_CTRL_EXT_SDP3_DATA            (1U << 7)
#define EM_MAIN_REG_CTRL_EXT_SDP7_DATA            (1U << 7)
/* SDP6[2] Pin Directionality */
#define EM_MAIN_REG_CTRL_EXT_SDP2_IODIR           (1U << 10)
#define EM_MAIN_REG_CTRL_EXT_SDP6_IODIR           (1U << 10)
/* SDP7[3] Pin Directionality */
#define EM_MAIN_REG_CTRL_EXT_SDP3_IODIR           (1U << 11)
#define EM_MAIN_REG_CTRL_EXT_SDP7_IODIR           (1U << 11)
/* ASD Check */
#define EM_MAIN_REG_CTRL_EXT_ASDCHK               (1U << 12)
/* EEPROM Reset */
#define EM_MAIN_REG_CTRL_EXT_EE_RST               (1U << 13)
/* Speed Select Bypass */
#define EM_MAIN_REG_CTRL_EXT_SPD_BYPS             (1U << 15)
/* Relaxed Ordering Disabled */
#define EM_MAIN_REG_CTRL_EXT_RO_DIS               (1U << 17)
/* Voltage Regulator Power Down */
#define EM_MAIN_REG_CTRL_EXT_VREG POWER_DOWN      (1U << 21)
/* Link Mode */
#define EM_MAIN_REG_CTRL_EXT_LINK_MODE_MASK       (3U << 21)
#define EM_MAIN_REG_CTRL_EXT_LINK_MODE_COBBER     (0U << 21)
#define EM_MAIN_REG_CTRL_EXT_LINK_MODE_FIBER      (2U << 21)
#define EM_MAIN_REG_CTRL_EXT_LINK_MODE_TBI        (3U << 21)

/* MDI Control */
#define EM_MAIN_REG_MDIC                0x0020
/* Data */
#define EM_MAIN_REG_MDIC_DATA_MASK                (0xFFFFU << 0)
#define EM_MAIN_REG_MDIC_DATA_SHIFT               0
/* PHY Register Address */
#define EM_MAIN_REG_MDIC_REGADD_MASK              (0x1FU << 16)
#define EM_MAIN_REG_MDIC_REGADD_SHIFT             16
/* PHY Address */
#define EM_MAIN_REG_MDIC_PHYADD_MASK              (0x1FU << 21)
#define EM_MAIN_REG_MDIC_PHYADD_PHY_ONE_AND_ONLY  (0x01U << 21)
/* Opcode */
#define EM_MAIN_REG_MDIC_OP_MASK                  (3U << 26)
#define EM_MAIN_REG_MDIC_OP_WRITE                 (1U << 26)
#define EM_MAIN_REG_MDIC_OP_READ                  (2U << 26)
/* Ready */
#define EM_MAIN_REG_MDIC_R                        (1U << 28)
/* Interrupt Enable */
#define EM_MAIN_REG_MDIC_I                        (1U << 29)
/* Error */
#define EM_MAIN_REG_MDIC_E                        (1U << 30)

/* Flow Control Address Low */
#define EM_MAIN_REG_FCAL                0x0028

/* Flow Control Address High */
#define EM_MAIN_REG_FCAH                0x002C

/* Flow Control Type */
#define EM_MAIN_REG_FCT                 0x0030

/* VLAN EtherType */
#define EM_MAIN_REG_VET                 0x0038

/* Flow Control Transmit Timer Value */
#define EM_MAIN_REG_FCTTV               0x0170

/* Transmit Configuration Word (not applicable to the 82540EP/EM, 82541xx and
   82547GI/EI) */
#define EM_MAIN_REG_TXCW                0x0170

/* Receive Configuration Word (not applicable to the 82540EP/EM, 82541xx and
   82547GI/EI) */
#define EM_MAIN_REG_RXCW                0x0180

/* When writing to this register pair, always set/clear in correct order */
/* Receive Address Low - base address (clear last, set first) */
#define EM_FILTER_REG_RAL               0x5400
/* Receive Address High - base address (clear first, set last) */
#define EM_FILTER_REG_RAH               0x5404

#define EM_EEPROM_REG_ETHERNET_ADDR_1   0x00
#define EM_EEPROM_REG_ETHERNET_ADDR_2   0x01
#define EM_EEPROM_REG_ETHERNET_ADDR_3   0x02

/* PHY Control Register */
#define EM_PHY_REG_PCTRL                0
/* Speed Selection (MSB) */
#define EM_PHY_REG_PCTRL_MSB                      (1U<<6)
/* Collision Test */
#define EM_PHY_REG_PCTRL_COLLISION_TEST           (1U<<7)
/* Duplex Mode */
#define EM_PHY_REG_PCTRL_DUPLEX                   (1U<<8)
/* Restart Auto-Negotiation */
#define EM_PHY_REG_PCTRL_RAN                      (1U<<9)
/* Isolate */
#define EM_PHY_REG_PCTRL_ISOLATE                  (1U<<10)
/* Power down */
#define EM_PHY_REG_PCTRL_POWER_DOWN               (1U<<11)
/* Auto-Negotiation Enable */
#define EM_PHY_REG_PCTRL_ANE                      (1U<<12)
/* Speed Selection (LSB) */
#define EM_PHY_REG_PCTRL_LSB                      (1U<<13)
/* Loopback */
#define EM_PHY_REG_PCTRL_LOOPBACK                 (1U<<14)
/* Reset */
#define EM_PHY_REG_PCTRL_RESET                    (1U<<15)
/* Speed Selection */
#define EM_PHY_REG_PCTRL_SPEED_MASK               (1U<<6 | 1U<<13)
#define EM_PHY_REG_PCTRL_SPEED_10_MBPS            (0U<<6 | 0U<<13)
#define EM_PHY_REG_PCTRL_SPEED_100_MBPS           (0U<<6 | 1U<<13)

/* PHY Status Register */
#define EM_PHY_REG_PSTATUS              1
/* Extended Capability */
#define EM_PHY_REG_PSTATUS_EXTENDED_CAPABILITY    (1U<<0)
/* Jabber Detect */
#define EM_PHY_REG_PSTATUS_JABBER_DETECT          (1U<<1)
/* Link Status */
#define EM_PHY_REG_PSTATUS_LINK_STATUS            (1U<<2)
/* Auto-Negotiation Ability */
#define EM_PHY_REG_PSTATUS_ANA                    (1U<<3)
/* Remote Fault */
#define EM_PHY_REG_PSTATUS_REMOTE_FAULT           (1U<<4)
/* Auto-Negotiation Complete */
#define EM_PHY_REG_PSTATUS_AN_COMPLETE            (1U<<5)
/* MF Preamble Suppression */
#define EM_PHY_REG_PSTATUS_MF_PREAMBLE_SUPP       (1U<<6)
/* Extended Status */
#define EM_PHY_REG_PSTATUS_EXTENDED_STATUS        (1U<<8)
/* 100BASE-T2 Half Duplex (not able to) */
#define EM_PHY_REG_PSTATUS_HD_100BASE_T2          (1U<<9)
/* 100BASE-T2 Full Duplex (not able to) */
#define EM_PHY_REG_PSTATUS_FD_100BASE_T2          (1U<<10)
/* 10 Mb/s Half Duplex (able to) */
#define EM_PHY_REG_PSTATUS_HD_10BASE_T            (1U<<11)
/* 10 Mb/s Fill Duplex (able to) */
#define EM_PHY_REG_PSTATUS_FD_10BASE_T            (1U<<12)
/* 100BASE-X Half Duplex (able to) */
#define EM_PHY_REG_PSTATUS_HD_100BASE_X           (1U<<13)
/* 100BASE-X Half Duplex (able to) */
#define EM_PHY_REG_PSTATUS_FD_100BASE_X           (1U<<14)
/* 100BASE-T4 (not able to) */
#define EM_PHY_REG_PSTATUS_100BASE_T4             (1U<<15)

/* PHY Specific Status Register */
#define EM_PHY_REG_PSSTAT               17
/* Jabber (real time) */
#define EM_PHY_REG_PSSTAT_JABBER                  (1U<<0)
/* Polarity (real time) */
#define EM_PHY_REG_PSSTAT_POLARITY                (1U<<1)
/* Receive Pause Enable */
#define EM_REG_PHY_PSSTAT_RECEIVE_PAUSE_ENABLE    (1U<<2)
/* Transmit Pause Enabled */
#define EM_REG_PHY_PSSTAT_TRANSMIT_PAUSE_ENABLE   (1U<<3)
/* Energy Detect Status */
#define EM_REG_PHY_PSSTAT_ENERGY_DETECT_STATUS    (1U<<4)
/* Downshift Status */
#define EM_REG_PHY_PSSTAT_DOWNSHIFT               (1U<<5)
/* MDI Crossover Status */
#define EM_PHY_REG_PSSTAT_MDI_CROSSOVER_STATUS    (1U<<6)
/* Cable Length (100/1000 modes only) */
#define EM_PHY_REG_PSSTAT_CABLE_LENGTH_MASK       (7U<<7)
#define EM_PHY_REG_PSSTAT_CABLE_LENGTH_0M_50M     (0U<<7)
#define EM_PHY_REG_PSSTAT_CABLE_LENGTH_50M_80M    (1U<<7)
#define EM_PHY_REG_PSSTAT_CABLE_LENGTH_80M_110M   (2U<<7)
#define EM_PHY_REG_PSSTAT_CABLE_LENGTH_110M_140M  (3U<<7)
#define EM_PHY_REG_PSSTAT_CABLE_LENGTH_140M_INFM  (4U<<7)
/* Link (real time) */
#define EM_PHY_REG_PSSTAT_LINK                    (1U<<10)
/* Speed and Duplex Resolved */
#define EM_PHY_REG_PSSTAT_SPEED_DUPLEX_RESOLVED   (1U<<11)
/* Page Received */
#define EM_PHY_REG_PSSTAT_PAGE_RECEIVED           (1U<<12)
/* Duplex */
#define EM_PHY_REG_PSSTAT_DUPLEX                  (1U<<13)
/* Speed */
#define EM_PHY_REG_PSSTAT_SPEED_MASK              (3U<<14)
#define EM_PHY_REG_PSSTAT_SPEED_10MBS             (0U<<14)
#define EM_PHY_REG_PSSTAT_SPEED_100MBS            (1U<<14)
#define EM_PHY_REG_PSSTAT_SPEED_1000MBS           (2U<<14)

#define EM_MAIN_REG_ICR                 0x00c0U
#define EM_MAIN_REG_ITR                 0x00c4U
#define EM_MAIN_REG_ICS                 0x00c8U
#define EM_MAIN_REG_IMS                 0x00d0U
#define EM_MAIN_REG_IMC                 0x00d8U

/* Receive Control */
#define EM_MAIN_REG_RCTL                0x0100U
/* Enable */
#define EM_MAIN_REG_RCTL_EN                       (1U << 1)
/* Store Bad packets (with CRC errors), used by some protocols */
#define EM_MAIN_REG_RCTL_SBP                      (1U << 2)
/* Unicast Promiscuous */
#define EM_MAIN_REG_RCTL_UPE                      (1U << 3)
/* Multicast Promiscuous */
#define EM_MAIN_REG_RCTL_MPE                      (1U << 4)
/* Long Packet ( >1522 bytes) Enable */
#define EM_MAIN_REG_RCTL_LPE                      (1U << 5)
/* Loopback Mode (mask and enable value) */
#define EM_MAIN_REG_RCTL_LBM                      (3U << 6)
/* Receive Descriptor Minimum Threshold size (fraction) */
#define EM_MAIN_REG_RCTL_RDMTS_MASK               (3U << 8)
#define EM_MAIN_REG_RCTL_RDMTS_1_2                (0U << 8)
#define EM_MAIN_REG_RCTL_RDMTS_1_4                (1U << 8)
#define EM_MAIN_REG_RCTL_RDMTS_1_8                (2U << 8)
/* Multicast Offset (for filtering) */
#define EM_MAIN_REG_RCTL_MO_MASK                  (3U << 12)
#define EM_MAIN_REG_RCTL_MO_BIT36                 (0U << 12)
#define EM_MAIN_REG_RCTL_MO_BIT35                 (1U << 12)
#define EM_MAIN_REG_RCTL_MO_BIT34                 (2U << 12)
#define EM_MAIN_REG_RCTL_MO_BIT32                 (3U << 12)
/* Broadcast Accept Mode (enable) */
#define EM_MAIN_REG_RCTL_BAM                      (1U << 15)
/* Receive Buffer Size (including extended) */
#define EM_MAIN_REG_RCTL_BSIZE_MASK   ((1U << 25)|(3U << 16))
#define EM_MAIN_REG_RCTL_BSIZE_2048               (0U << 16)
#define EM_MAIN_REG_RCTL_BSIZE_1024               (1U << 16)
#define EM_MAIN_REG_RCTL_BSIZE_512                (2U << 16)
#define EM_MAIN_REG_RCTL_BSIZE_256                (3U << 16)
#define EM_MAIN_REG_RCTL_BSIZE_16384  ((1U << 25)|(1U << 16))
#define EM_MAIN_REG_RCTL_BSIZE_8192   ((1U << 25)|(2U << 16))
#define EM_MAIN_REG_RCTL_BSIZE_4096   ((1U << 25)|(3U << 16))
/* VLAN Filter Enable */
#define EM_MAIN_REG_RCTL_VFE                      (1U << 18)
/* Canonical Form Indicator Enable (for VLANs) */
#define EM_MAIN_REG_RCTL_CFIEN                    (1U << 19)
/* Canonical Form Indicator (for VLANs) */
#define EM_MAIN_REG_RCTL_CFI                      (1U << 20)
/* Discard Pause Frames (MAC Flow control) */
#define EM_MAIN_REG_RCTL_DPF                      (1U << 22)
/* Pass MAC Control Frames */
#define EM_MAIN_REG_RCTL_PMCF                     (1U << 23)
/* Strip CRC */
#define EM_MAIN_REG_RCTL_SECRC                    (1U << 26)

/* Receive Descriptor Base Low */
#define EM_MAIN_REG_RDBAL               0x2800U
/* Receive Descriptor Base High */
#define EM_MAIN_REG_RDBAH               0x2804U
/* Receive Descriptor Length (128 byte align minimum) */
#define EM_MAIN_REG_RDLEN               0x2808U
/* Receive Descriptor Head (Reading is not reliable) */
#define EM_MAIN_REG_RDH                 0x2810U
/* Receive Descriptor Tail */
#define EM_MAIN_REG_RDT                 0x2818U
/* Receive Delay Delay Timer (should usually be disabled) */
#define EM_MAIN_REG_RDTR                0x2820U
/* Receive Interrupt Absolute Delay Timer (should usually be disabled) */
#define EM_MAIN_REG_RADV                0x282cU
/* Receive Small Packet Detect Interrupt (size in bytes) */
#define EM_MAIN_REG_RSRPD               0x2c00U

/* Transmit Control */
#define EM_MAIN_REG_TCTL                0x0400U
#define EM_MAIN_REG_TCTL_EN                       (1U << 1)
#define EM_MAIN_REG_TCTL_PSP                      (1U << 3)
#define EM_MAIN_REG_TCTL_CT(v)                    (((v) & 0xff) << 4)
#define EM_MAIN_REG_TCTL_COLD(v)                  (((v) & 0x3ff) << 12)
#define EM_MAIN_REG_TCTL_SWXOFF                   (1U << 22)
#define EM_MAIN_REG_TCTL_RTLC                     (1U << 24)
#define EM_MAIN_REG_TCTL_RESERVED1                (1U << 28) /* TODO: Seems to be set as one */
#define EM_MAIN_REG_TCTL_RRTHRESH(v)              (((v) & 0x3) << 29)

/* Transmit IPG */
#define EM_MAIN_REG_TIPG                0x0410U
#define EM_MAIN_REG_TIPG_IPGT(v)                  (((v) & 0x3FF) << 0)
#define EM_MAIN_REG_TIPG_IPGT_MASK                (0x1FFU << 0)
#define EM_MAIN_REG_TIPG_IPGR1(v)                 (((v) & 0x3FF) << 10)
#define EM_MAIN_REG_TIPG_IPGR1_MASK               (0x3FFU << 10)
#define EM_MAIN_REG_TIPG_IPGR2(v)                 (((v) & 0x3FF) << 20)
#define EM_MAIN_REG_TIPG_IPGR2_MASK               (0x3FFU << 20)

/* TX DMA Control */
#define EM_MAIN_REG_TXDMAC              0x3000U
/* Transmit Descriptor Base Low and High */
#define EM_MAIN_REG_TDBAL               0x3800U
#define EM_MAIN_REG_TDBAH               0x3804U
/* Transmit Descriptors Length */
#define EM_MAIN_REG_TDLEN               0x3808U
/* Transmit Descriptor Head (Reading is not reliable) */
#define EM_MAIN_REG_TDH                 0x3810U
/* Transmit Descriptor Tail */
#define EM_MAIN_REG_TDT                 0x3818U

/* Transmit Descriptor Control */
#define EM_MAIN_REG_TXDCTL              0x3828U
#define EM_MAIN_REG_TXDCTL_PTHRESH(v) (((v) & 0x3FU) << 0)
#define EM_MAIN_REG_TXDCTL_HTHRESH(v) (((v) & 0x3FU) << 8)
#define EM_MAIN_REG_TXDCTL_WTHRESH(v) (((v) & 0x3FU) << 16)
#define EM_MAIN_REG_TXDCTL_GRAN (1U << 24)
#define EM_MAIN_REG_TXDCTL_LWTHRESH(v) (((v) & 0x7FU) << 25)

#define EM_INTERRUPT_TXDW                         (1U << 0)
#define EM_INTERRUPT_TXQE                         (1U << 1)
#define EM_INTERRUPT_LSC                          (1U << 2)
#define EM_INTERRUPT_RXSEQ                        (1U << 3)
#define EM_INTERRUPT_RXDMT0                       (1U << 4)
#define EM_INTERRUPT_RXO                          (1U << 6)
#define EM_INTERRUPT_RXT0                         (1U << 7)
#define EM_INTERRUPT_MDAC                         (1U << 9)
#define EM_INTERRUPT_RXCFG                        (1U <<10)
#define EM_INTERRUPT_PHYINT                       (1U <<12)
#define EM_INTERRUPT_GPI2                         (1U <<13)
#define EM_INTERRUPT_GPI3                         (1U <<14)
#define EM_INTERRUPT_TXD_LOW                      (1U <<15)
#define EM_INTERRUPT_SRPD                         (1U <<16)

#define EM_STAT_REG_CRCERRS             0x4000
#define EM_STAT_REG_ALGNERRC            0x4004
#define EM_STAT_REG_SYMERRS             0x4008
#define EM_STAT_REG_RXERRC              0x400c
#define EM_STAT_REG_MPC                 0x4010
#define EM_STAT_REG_SCC                 0x4014
#define EM_STAT_REG_ECOL                0x4018
#define EM_STAT_REG_MCC                 0x401c
#define EM_STAT_REG_LATECOL             0x4020
#define EM_STAT_REG_COLC                0x4028
#define EM_STAT_REG_DC                  0x4030
#define EM_STAT_REG_TNCRS               0x4034
#define EM_STAT_REG_SEC                 0x4038
#define EM_STAT_REG_CEXTERR             0x403c
#define EM_STAT_REG_RLEC                0x4040
#define EM_STAT_REG_XONRXC              0x4048
#define EM_STAT_REG_XONTXC              0x404c
#define EM_STAT_REG_XOFFRXC             0x4050
#define EM_STAT_REG_XOFFTXC             0x4054
#define EM_STAT_REG_FCRUC               0x4058
#define EM_STAT_REG_PRC64               0x405c
#define EM_STAT_REG_PRC127              0x4060
#define EM_STAT_REG_PRC255              0x4064
#define EM_STAT_REG_PRC511              0x4068
#define EM_STAT_REG_PRC1023             0x406c
#define EM_STAT_REG_PRC1522             0x4070
#define EM_STAT_REG_GPRC                0x4074
#define EM_STAT_REG_BPRC                0x4078
#define EM_STAT_REG_MPRC                0x407c
#define EM_STAT_REG_GPTC                0x4080
#define EM_STAT_REG_GORCL               0x4088
#define EM_STAT_REG_GORCH               0x408c
#define EM_STAT_REG_GOTCL               0x4090
#define EM_STAT_REG_GOTCH               0x4094
#define EM_STAT_REG_RNBC                0x40a0
#define EM_STAT_REG_RUC                 0x40a4
#define EM_STAT_REG_RFC                 0x40a8
#define EM_STAT_REG_ROC                 0x40ac
#define EM_STAT_REG_RJC                 0x40b0
#define EM_STAT_REG_MGTPRC              0x40b4
#define EM_STAT_REG_MGTPDC              0x40b8
#define EM_STAT_REG_MGTPTC              0x40bc
#define EM_STAT_REG_TORL                0x40c0
#define EM_STAT_REG_TORH                0x40c4
#define EM_STAT_REG_TOTL                0x40c8
#define EM_STAT_REG_TOTH                0x40cc
#define EM_STAT_REG_TPR                 0x40d0
#define EM_STAT_REG_TPT                 0x40d4
#define EM_STAT_REG_PTC64               0x40d8
#define EM_STAT_REG_PTC127              0x40dc
#define EM_STAT_REG_PTC255              0x40e0
#define EM_STAT_REG_PTC511              0x40e4
#define EM_STAT_REG_PTC1023             0x40e8
#define EM_STAT_REG_PTC1522             0x40ec
#define EM_STAT_REG_MPTC                0x40f0
#define EM_STAT_REG_BPTC                0x40f4
#define EM_STAT_REG_TSCTC               0x40f8
#define EM_STAT_REG_TSCTFC              0x40fc

#define EM_TDESC_TYPE_TCPDATA   ((1U << 20) | (1U << 29))
#define EM_TDESC_CMD_EOP        (1U << 24)
#define EM_TDESC_CMD_IFCS       (1U << 25)
#define EM_TDESC_CMD_TSE        (1U << 26)
#define EM_TDESC_CMD_RS         (1U << 27)
#define EM_TDESC_CMD_VLE        (1U << 30)
#define EM_TDESC_CMD_IDE        (1U << 31)
#define EM_TDESC_LENGTH(l)      ((l) & 0xfffff)

#endif
