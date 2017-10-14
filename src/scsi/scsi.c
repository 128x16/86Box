/*
 * 86Box	A hypervisor and IBM PC system emulator that specializes in
 *		running old operating systems and software designed for IBM
 *		PC systems and compatibles from 1981 through fairly recent
 *		system designs based on the PCI bus.
 *
 *		This file is part of the 86Box distribution.
 *
 *		Handling of the SCSI controllers.
 *
 * Version:	@(#)scsi.c	1.0.9	2017/10/10
 *
 * Authors:	TheCollector1995, <mariogplayer@gmail.com>
 *		Miran Grca, <mgrca8@gmail.com>
 *		Fred N. van Kempen, <decwiz@yahoo.com>
 *		Copyright 2016,2017 Miran Grca.
 *		Copyright 2017 Fred N. van Kempen.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "../86box.h"
#include "../ibm.h"
#include "../mem.h"
#include "../rom.h"
#include "../timer.h"
#include "../device.h"
#include "../cdrom/cdrom.h"
#include "../disk/hdc.h"
#include "../plat.h"
#include "scsi.h"
#include "scsi_aha154x.h"
#include "scsi_buslogic.h"
#include "scsi_ncr5380.h"
#include "scsi_x54x.h"


scsi_device_t	SCSIDevices[SCSI_ID_MAX][SCSI_LUN_MAX];
uint8_t		SCSIPhase = 0xff;
uint8_t		SCSIStatus = SCSI_STATUS_OK;
uint8_t		scsi_cdrom_id = 3; /*common setting*/
char		scsi_fn[SCSI_NUM][512];
uint16_t	scsi_hd_location[SCSI_NUM];

int		scsi_card_current = 0;
int		scsi_card_last = 0;

uint32_t	SCSI_BufferLength;
void		*scsiMutex;


typedef struct {
    char	name[64];
    char	internal_name[32];
    device_t	*device;
    void	(*reset)(void *p);
} SCSI_CARD;


static SCSI_CARD scsi_cards[] = {
    { "None",			"none",		NULL,		      NULL		  },
    { "[ISA] Adaptec AHA-1540B","aha1540b",	&aha1540b_device,     x54x_device_reset   },
    { "[ISA] Adaptec AHA-1542C","aha1542c",	&aha1542c_device,     x54x_device_reset   },
    { "[ISA] Adaptec AHA-1542CF","aha1542cf",	&aha1542cf_device,    x54x_device_reset   },
    { "[ISA] BusLogic BT-542BH","bt542bh",	&buslogic_device,     BuslogicDeviceReset },
    { "[ISA] BusLogic BT-545S",	"bt545s",	&buslogic_545s_device,BuslogicDeviceReset },
    { "[ISA] Longshine LCS-6821N","lcs6821n",	&scsi_lcs6821n_device,NULL		  },
    { "[ISA] Ranco RT1000B",	"rt1000b",	&scsi_rt1000b_device, NULL		  },
    { "[ISA] Trantor T130B",	"t130b",	&scsi_t130b_device,   NULL		  },
    { "[MCA] Adaptec AHA-1640",	"aha1640",	&aha1640_device,      x54x_device_reset   },
    { "[PCI] BusLogic BT-958D",	"bt958d",	&buslogic_pci_device, BuslogicDeviceReset },
    { "",			"",		NULL,		      NULL		  },
};


int scsi_card_available(int card)
{
    if (scsi_cards[card].device)
	return(device_available(scsi_cards[card].device));

    return(1);
}


char *scsi_card_getname(int card)
{
    return(scsi_cards[card].name);
}


device_t *scsi_card_getdevice(int card)
{
    return(scsi_cards[card].device);
}


int scsi_card_has_config(int card)
{
    if (! scsi_cards[card].device) return(0);

    return(scsi_cards[card].device->config ? 1 : 0);
}


char *scsi_card_get_internal_name(int card)
{
    return(scsi_cards[card].internal_name);
}


int scsi_card_get_from_internal_name(char *s)
{
    int c = 0;

    while (strlen(scsi_cards[c].internal_name)) {
	if (!strcmp(scsi_cards[c].internal_name, s))
		return(c);
	c++;
    }
	
    return(0);
}


void scsi_mutex_init(void)
{
    scsiMutex = thread_create_mutex(L"86Box.SCSIMutex");
}


void scsi_mutex_close(void)
{
    thread_close_mutex(scsiMutex);
}


void scsi_card_init(void)
{
    int i, j;

    pclog("Building SCSI hard disk map...\n");
    build_scsi_hd_map();
    pclog("Building SCSI CD-ROM map...\n");
    build_scsi_cdrom_map();
	
    for (i=0; i<SCSI_ID_MAX; i++) {
	for (j=0; j<SCSI_LUN_MAX; j++) {
		if (scsi_hard_disks[i][j] != 0xff) {
			SCSIDevices[i][j].LunType = SCSI_DISK;
		} else if (scsi_cdrom_drives[i][j] != 0xff) {
			SCSIDevices[i][j].LunType = SCSI_CDROM;
		} else {
			SCSIDevices[i][j].LunType = SCSI_NONE;
		}
		SCSIDevices[i][j].CmdBuffer = NULL;
	}
    }

    if (scsi_cards[scsi_card_current].device)
	device_add(scsi_cards[scsi_card_current].device);

    scsi_card_last = scsi_card_current;
}


void scsi_card_reset(void)
{
    void *p = NULL;

    if (scsi_cards[scsi_card_current].device) {
	p = device_get_priv(scsi_cards[scsi_card_current].device);
	if (p) {
		if (scsi_cards[scsi_card_current].reset) {
			scsi_cards[scsi_card_current].reset(p);
		}
	}
    }
}


/* Initialization function for the SCSI layer */
void SCSIReset(uint8_t id, uint8_t lun)
{
    uint8_t cdrom_id = scsi_cdrom_drives[id][lun];
    uint8_t hdc_id = scsi_hard_disks[id][lun];

    if (hdc_id != 0xff) {
	scsi_hd_reset(cdrom_id);
	SCSIDevices[id][lun].LunType = SCSI_DISK;
    } else {
	if (cdrom_id != 0xff) {
		cdrom_reset(cdrom_id);
		SCSIDevices[id][lun].LunType = SCSI_CDROM;
	} else {
		SCSIDevices[id][lun].LunType = SCSI_NONE;
	}
    }

    if (SCSIDevices[id][lun].CmdBuffer != NULL) {
	free(SCSIDevices[id][lun].CmdBuffer);
	SCSIDevices[id][lun].CmdBuffer = NULL;
    }
}


void
startscsi(void)
{
    thread_wait_mutex(scsiMutex);
}


void
endscsi(void)
{
    thread_release_mutex(scsiMutex);
}
