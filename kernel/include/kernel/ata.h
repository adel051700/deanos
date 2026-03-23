#ifndef _KERNEL_ATA_H
#define _KERNEL_ATA_H

typedef struct ata_probe_summary {
	unsigned int slots_tested;
	unsigned int devices_present;
	unsigned int ata_devices;
	unsigned int atapi_devices;
	unsigned int devices_registered;
	unsigned int register_failures;
} ata_probe_summary_t;

void ata_initialize(void);
void ata_probe_get_summary(ata_probe_summary_t* out_summary);

#endif

