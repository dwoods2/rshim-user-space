/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 *
 * Copyright 2019 Mellanox Technologies. All Rights Reserved.
 *
 */

#include <sys/epoll.h>
#include <sys/mman.h>
#include <pci/pci.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <pthread.h>

#include "rshim.h"

/** Our Vendor/Device IDs. */
#define TILERA_VENDOR_ID            0x15b3
#define BLUEFIELD_DEVICE_ID         0x0211

/* Mellanox Address & Data Capabilities */
#define MELLANOX_ADDR               0x58
#define MELLANOX_DATA               0x5c
#define MELLANOX_CAP_READ           0x1

/* TRIO_CR_GATEWAY registers */
#define TRIO_CR_GW_LOCK             0xe38a0
#define TRIO_CR_GW_LOCK_CPY         0xe38a4
#define TRIO_CR_GW_DATA_UPPER       0xe38ac
#define TRIO_CR_GW_DATA_LOWER       0xe38b0
#define TRIO_CR_GW_CTL              0xe38b4
#define TRIO_CR_GW_ADDR_UPPER       0xe38b8
#define TRIO_CR_GW_ADDR_LOWER       0xe38bc
#define TRIO_CR_GW_LOCK_ACQUIRED    0x80000000
#define TRIO_CR_GW_LOCK_RELEASE     0x0
#define TRIO_CR_GW_BUSY             0x60000000
#define TRIO_CR_GW_TRIGGER          0xe0000000
#define TRIO_CR_GW_READ_4BYTE       0x6
#define TRIO_CR_GW_WRITE_4BYTE      0x2

/* Base RShim Address */
#define RSH_BASE_ADDR               0x80000000
#define RSH_CHANNEL1_BASE           0x80010000

struct rshim_pcie {
  /* RShim backend structure. */
  struct rshim_backend bd;

  struct pci_dev *pci_dev;

  /* Keep track of number of 8-byte word writes */
  u8 write_count;
};

/* Mechanism to access the CR space using hidden PCI capabilities */
static int pci_cap_read(struct pci_dev *pci_dev, int offset, uint32_t *result)
{
  int rc;

  /*
   * Write target offset to MELLANOX_ADDR.
   * Set LSB to indicate a read operation.
   */
  rc = pci_write_long(pci_dev, MELLANOX_ADDR, offset | MELLANOX_CAP_READ);
  if (rc < 0)
    return rc;

  /* Read result from MELLANOX_DATA */
  *result = pci_read_long(pci_dev, MELLANOX_DATA);

  return 0;
}

static int pci_cap_write(struct pci_dev *pci_dev, int offset, uint32_t value)
{
  int rc;

  /* Write data to MELLANOX_DATA */
  rc = pci_write_long(pci_dev, MELLANOX_DATA, value);
  if (rc < 0)
    return rc;

  /*
   * Write target offset to MELLANOX_ADDR.
   * Leave LSB clear to indicate a write operation.
   */
  rc = pci_write_long(pci_dev, MELLANOX_ADDR, offset);
  if (rc < 0)
    return rc;

  return 0;
}

/* Acquire and release the TRIO_CR_GW_LOCK. */
static int trio_cr_gw_lock_acquire(struct pci_dev *pci_dev)
{
  uint32_t read_value;
  int rc;

  /* Wait until TRIO_CR_GW_LOCK is free */
  do {
    rc = pci_cap_read(pci_dev, TRIO_CR_GW_LOCK, &read_value);
    if (rc)
      return rc;
  } while (read_value & TRIO_CR_GW_LOCK_ACQUIRED);

  /* Acquire TRIO_CR_GW_LOCK */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_LOCK, TRIO_CR_GW_LOCK_ACQUIRED);
  if (rc)
    return rc;

  return 0;
}

static int trio_cr_gw_lock_release(struct pci_dev *pci_dev)
{
  int rc;

  /* Release TRIO_CR_GW_LOCK */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_LOCK, TRIO_CR_GW_LOCK_RELEASE);

  return rc;
}

/*
 * Mechanism to access the RShim from the CR space using the TRIO_CR_GATEWAY.
 */
static int trio_cr_gw_read(struct pci_dev *pci_dev, int addr, uint32_t *result)
{
  int rc;

  /* Acquire TRIO_CR_GW_LOCK */
  rc = trio_cr_gw_lock_acquire(pci_dev);
  if (rc)
    return rc;

  /* Write addr to TRIO_CR_GW_ADDR_LOWER */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_ADDR_LOWER, addr);
  if (rc)
    return rc;

  /* Set TRIO_CR_GW_READ_4BYTE */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_CTL, TRIO_CR_GW_READ_4BYTE);
  if (rc)
    return rc;

  /* Trigger TRIO_CR_GW to read from addr */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_LOCK, TRIO_CR_GW_TRIGGER);
  if (rc)
    return rc;

  /* Read 32-bit data from TRIO_CR_GW_DATA_LOWER */
  rc = pci_cap_read(pci_dev, TRIO_CR_GW_DATA_LOWER, result);
  if (rc)
    return rc;

  /* Release TRIO_CR_GW_LOCK */
  rc = trio_cr_gw_lock_release(pci_dev);
  if (rc)
    return rc;

  return 0;
}

static int trio_cr_gw_write(struct pci_dev *pci_dev, int addr, uint32_t value)
{
  int rc;

  /* Acquire TRIO_CR_GW_LOCK */
  rc = trio_cr_gw_lock_acquire(pci_dev);
  if (rc)
    return rc;

  /* Write 32-bit data to TRIO_CR_GW_DATA_LOWER */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_DATA_LOWER, value);
  if (rc)
    return rc;

  /* Write addr to TRIO_CR_GW_ADDR_LOWER */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_ADDR_LOWER, addr);
  if (rc)
    return rc;

  /* Set TRIO_CR_GW_WRITE_4BYTE */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_CTL, TRIO_CR_GW_WRITE_4BYTE);
  if (rc)
    return rc;

  /* Trigger CR gateway to write to RShim */
  rc = pci_cap_write(pci_dev, TRIO_CR_GW_LOCK, TRIO_CR_GW_TRIGGER);
  if (rc)
    return rc;

  /* Release TRIO_CR_GW_LOCK */
  rc = trio_cr_gw_lock_release(pci_dev);
  if (rc)
    return rc;

  return 0;
}

/* Wait until the RSH_BYTE_ACC_CTL pending bit is cleared */
static int rshim_byte_acc_pending_wait(struct pci_dev *pci_dev)
{
  uint32_t read_value;
  int rc;

  do {
    rc = trio_cr_gw_read(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_CTL,
                         &read_value);
    if (rc)
      return rc;
  } while (read_value & (RSH_CHANNEL1_BASE + RSH_BYTE_ACC_PENDING));

  return 0;
}

/*
 * Mechanism to do an 8-byte access to the Rshim using
 * two 4-byte accesses through the Rshim Byte Access Widget.
 */
static int rshim_byte_acc_read(struct pci_dev *pci_dev, int addr,
                               uint64_t *result)
{
  uint64_t read_result;
  uint32_t read_value;
  int rc;

  /* Wait for RSH_BYTE_ACC_CTL pending bit to be cleared */
  rc = rshim_byte_acc_pending_wait(pci_dev);
  if (rc)
    return rc;

  /* Write control bits to RSH_BYTE_ACC_CTL */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_CTL,
                        RSH_BYTE_ACC_SIZE);
  if (rc)
    return rc;

  /* Write target address to RSH_BYTE_ACC_ADDR */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_ADDR,
        addr);
  if (rc)
    return rc;

  /* Write trigger bits to perform read */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_CTL,
        RSH_BYTE_ACC_READ_TRIGGER);
  if (rc)
    return rc;

  /* Wait for RSH_BYTE_ACC_CTL pending bit to be cleared */
  rc = rshim_byte_acc_pending_wait(pci_dev);
  if (rc)
    return rc;

  /* Read RSH_BYTE_ACC_RDAT to read lower 32-bits of data */
  rc = trio_cr_gw_read(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_RDAT,
                       &read_value);
  if (rc)
    return rc;

  read_result = (uint64_t)read_value << 32;

  /* Wait for RSH_BYTE_ACC_CTL pending bit to be cleared */
  rc = rshim_byte_acc_pending_wait(pci_dev);
  if (rc)
    return rc;

  /* Read RSH_BYTE_ACC_RDAT to read upper 32-bits of data */
  rc = trio_cr_gw_read(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_RDAT,
                       &read_value);
  if (rc)
    return rc;

  read_result |= (uint64_t)read_value;
  *result = be64toh(read_result);

  return 0;
}

static int rshim_byte_acc_write(struct pci_dev *pci_dev, int addr,
                                uint64_t value)
{
  int rc;

  /* Wait for RSH_BYTE_ACC_CTL pending bit to be cleared */
  rc = rshim_byte_acc_pending_wait(pci_dev);
  if (rc)
    return rc;

  /* Write control bits to RSH_BYTE_ACC_CTL */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_CTL,
                        RSH_BYTE_ACC_SIZE);
  if (rc)
    return rc;

  /* Write target address to RSH_BYTE_ACC_ADDR */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_ADDR,
        addr);
  if (rc)
    return rc;

  /* Write control bits to RSH_BYTE_ACC_CTL */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_CTL,
                        RSH_BYTE_ACC_SIZE);
  if (rc)
    return rc;

  /* Write lower 32 bits of data to TRIO_CR_GW_DATA */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_WDAT,
                        (uint32_t)(value >> 32));
  if (rc)
    return rc;

  /* Wait for RSH_BYTE_ACC_CTL pending bit to be cleared */
  rc = rshim_byte_acc_pending_wait(pci_dev);
  if (rc)
    return rc;

  /* Write upper 32 bits of data to TRIO_CR_GW_DATA */
  rc = trio_cr_gw_write(pci_dev, RSH_CHANNEL1_BASE + RSH_BYTE_ACC_WDAT,
                        (uint32_t)(value));
  if (rc)
    return rc;

  return 0;
}

/*
 * The RShim Boot FIFO has a holding register which can couple
 * two consecutive 4-byte writes into a single 8-byte write
 * before pushing the data into the FIFO.
 * Hence the RShim Byte Access Widget is not necessary to write
 * to the BOOT FIFO using 4-byte writes.
 */
static int rshim_boot_fifo_write(struct pci_dev *pci_dev, int addr,
                                 uint64_t value)
{
  int rc;

  /* Write lower 32 bits of data to RSH_BOOT_FIFO_DATA */
  rc = trio_cr_gw_write(pci_dev, addr, (uint32_t)(value >> 32));
  if (rc)
    return rc;

  /* Write upper 32 bits of data to RSH_BOOT_FIFO_DATA */
  rc = trio_cr_gw_write(pci_dev, addr, (uint32_t)(value));
  if (rc)
    return rc;

  return 0;
}

/* RShim read/write routines */
static int rshim_pcie_read(struct rshim_backend *bd, int chan, int addr,
                           uint64_t *result)
{
  struct rshim_pcie *dev = container_of(bd, struct rshim_pcie, bd);
  struct pci_dev *pci_dev = dev->pci_dev;
  int rc = 0;

  if (!bd->has_rshim)
    return -ENODEV;

  dev->write_count = 0;

  addr = RSH_BASE_ADDR + (addr | (chan << 16));
  addr = be32toh(addr);

  rc = rshim_byte_acc_read(pci_dev, addr, result);

  return rc;

}

static int rshim_pcie_write(struct rshim_backend *bd, int chan, int addr,
                            uint64_t value)
{
  struct rshim_pcie *dev = container_of(bd, struct rshim_pcie, bd);
  struct pci_dev *pci_dev = dev->pci_dev;
  bool is_boot_stream = (addr == RSH_BOOT_FIFO_DATA);
  uint64_t result;
  int rc = 0;

  if (!bd->has_rshim)
    return -ENODEV;

  addr = RSH_BASE_ADDR + (addr | (chan << 16));
  if (!is_boot_stream)
    addr = be32toh(addr);

  value = be64toh(value);

  /*
   * We cannot stream large numbers of PCIe writes to the RShim's BAR.
   * Instead, we must write no more than 15 8-byte words before
   * doing a read from another register within the BAR,
   * which forces previous writes to drain.
   * Note that we allow a max write_count of 7 since each 8-byte
   * write is done using 2 4-byte writes in the boot fifo case.
   */
  if (dev->write_count == 7) {
    __sync_synchronize();
    rshim_pcie_read(bd, chan, RSH_SCRATCHPAD, &result);
  }
  dev->write_count++;

  if (is_boot_stream)
    rc = rshim_boot_fifo_write(pci_dev, addr, value);
  else
    rc = rshim_byte_acc_write(pci_dev, addr, value);

  return rc;
}

static void rshim_pcie_delete(struct rshim_backend *bd)
{
  struct rshim_pcie *dev = container_of(bd, struct rshim_pcie, bd);

  rshim_deregister(bd);
  free(dev);
}

/* Probe routine */
static int rshim_pcie_probe(struct pci_dev *pci_dev)
{
  const int max_name_len = 64;
  int ret, allocfail = 0;
  struct rshim_backend *bd;
  struct rshim_pcie *dev;
  char *pcie_dev_name;
  pciaddr_t bar0;

  pcie_dev_name = malloc(max_name_len);
  snprintf(pcie_dev_name, max_name_len, "pcie-%d-%d-%d-%d",
           pci_dev->domain_16, pci_dev->bus, pci_dev->dev, pci_dev->func);

  RSHIM_INFO("Probing %s\n", pcie_dev_name);

  rshim_lock();

  bd = rshim_find_by_name(pcie_dev_name);
  if (bd) {
    dev = container_of(bd, struct rshim_pcie, bd);
  } else {
    dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
      ret = -ENOMEM;
      rshim_unlock();
      goto error;
    }

    bd = &dev->bd;
    bd->has_rshim = 1;
    bd->has_tm = 1;
    bd->dev_name = pcie_dev_name;
    bd->drv_name = "rshim_pcie_lf";
    bd->read_rshim = rshim_pcie_read;
    bd->write_rshim = rshim_pcie_write;
    bd->destroy = rshim_pcie_delete;
    dev->write_count = 0;
    pthread_mutex_init(&bd->mutex, NULL);
  }

  ret = rshim_fifo_alloc(bd);
  if (ret) {
    rshim_unlock();
    RSHIM_ERR("Failed to allocate fifo\n");
    ret = -ENOMEM;
    goto error;
  }

  allocfail |= rshim_fifo_alloc(bd);

  if (!bd->read_buf)
    bd->read_buf = calloc(1, READ_BUF_SIZE);
  allocfail |= bd->read_buf == 0;

  if (!bd->write_buf)
    bd->write_buf = calloc(1, WRITE_BUF_SIZE);
  allocfail |= bd->write_buf == 0;

  if (allocfail) {
    rshim_unlock();
    RSHIM_ERR("can't allocate buffers\n");
    ret = -ENOMEM;
    goto error;
  }

  rshim_unlock();

  /* Initialize object */
  dev->pci_dev = pci_dev;

  /*
   * Register rshim here since it needs to detect whether other backend
   * has already registered or not, which involves reading/writting rshim
   * registers and has assumption that the under layer is working.
   */
  rshim_lock();
  if (!bd->registered) {
    ret = rshim_register(bd);
    if (ret) {
      rshim_unlock();
      goto rshim_map_failed;
    } else {
      pcie_dev_name = NULL;
    }
  }
  rshim_unlock();

  /* Notify that the device is attached */
  pthread_mutex_lock(&bd->mutex);
  ret = rshim_notify(bd, RSH_EVENT_ATTACH, 0);
  pthread_mutex_unlock(&bd->mutex);
  if (ret)
    goto rshim_map_failed;

  return 0;

 rshim_map_failed:
 error:
   free(pcie_dev_name);
   return ret;
}

#if 0
/* Called via pci_unregister_driver() when the module is removed. */
static void rshim_pcie_remove(struct pci_dev *pci_dev)
{
  struct rshim_pcie *dev = dev_get_drvdata(&pci_dev->dev);
  int rc, flush_wq;

  /*
   * Reset TRIO_PCIE_INTFC_RX_BAR0_ADDR_MASK and TRIO_MAP_RSH_BASE.
   * Otherwise, upon host reboot, the two registers will retain previous
   * values that don't match the new BAR0 address that is assigned to
   * the PCIe ports, causing host MMIO access to RShim to fail.
   */
  rc = rshim_pcie_write(&dev->bd, (RSH_SWINT >> 16) & 0xF,
                        RSH_SWINT & 0xFFFF, RSH_INT_VEC0_RTC__SWINT3_MASK);
  if (rc)
    ERROR("RShim write failed");

  /* Clear the flags before deleting the backend. */
  dev->bd.has_rshim = 0;
  dev->bd.has_tm = 0;

  rshim_notify(&dev->bd, RSH_EVENT_DETACH, 0);
  mutex_lock(&dev->bd.mutex);
  flush_wq = !cancel_delayed_work(&dev->bd.work);
  if (flush_wq)
    flush_workqueue(rshim_wq);
  dev->bd.has_cons_work = 0;
  kfree(dev->bd.read_buf);
  kfree(dev->bd.write_buf);
  rshim_fifo_free(&dev->bd);
  mutex_unlock(&dev->bd.mutex);

  rshim_lock();
  kref_put(&dev->bd.kref, rshim_pcie_delete);
  rshim_unlock();

  pci_disable_device(pci_dev);
  dev_set_drvdata(&pci_dev->dev, NULL);
}
#endif

int rshim_pcie_lf_init(void)
{
  struct pci_access *pci;
  struct pci_dev *dev;

  pci = pci_alloc();
  if (!pci)
    return -ENOMEM;

  pci_init(pci);

  pci_scan_bus(pci);

  /* Iterate over the devices */
  for (dev = pci->devices; dev; dev = dev->next) {
    pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_CLASS);

    if (dev->vendor_id != TILERA_VENDOR_ID ||
        dev->device_id != BLUEFIELD_DEVICE_ID)
      continue;

    rshim_pcie_probe(dev);
  }

  //pci_cleanup(pci);

  return 0;
}

void rshim_pcie_lf_exit(void)
{
}
