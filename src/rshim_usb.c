// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 Mellanox Technologies. All Rights Reserved.
 *
 */

#include <libusb.h>
#include <string.h>
#include <poll.h>
#include <sys/epoll.h>
#include <pthread.h>

#include "rshim.h"

/* Our USB vendor/product IDs. */
#define USB_TILERA_VENDOR_ID        0x22dc   /* Tilera Corporation */
#define USB_BLUEFIELD_1_PRODUCT_ID  0x0004   /* Mellanox Bluefield-1 */
#define USB_BLUEFIELD_2_PRODUCT_ID  0x0214   /* Mellanox Bluefield-2 */

#define READ_RETRIES       5
#define WRITE_RETRIES      5
#define RSHIM_USB_TIMEOUT  20000

/* Structure to hold all of our device specific stuff. */
typedef struct {
  /* RShim backend structure. */
  rshim_backend_t bd;

  libusb_device_handle *handle;

  /* Control data. */
  uint64_t ctrl_data;

  /* Interrupt data buffer.  This is a USB DMA'able buffer. */
  uint64_t *intr_buf;

  /* Read/interrupt urb, retries, and mode. */
  struct libusb_transfer *read_or_intr_urb;
  int read_or_intr_retries;
  int read_urb_is_intr;

  /* Write urb and retries. */
  struct libusb_transfer *write_urb;
  int write_retries;

  /* The address of the boot FIFO endpoint. */
  uint8_t boot_fifo_ep;
  /* The address of the tile-monitor FIFO interrupt endpoint. */
  uint8_t tm_fifo_int_ep;
  /* The address of the tile-monitor FIFO input endpoint. */
  uint8_t tm_fifo_in_ep;
  /* The address of the tile-monitor FIFO output endpoint. */
  uint8_t tm_fifo_out_ep;
} rshim_usb_t;

static libusb_context *rshim_usb_ctx;
static int rshim_usb_epoll_fd;
static bool rshim_usb_need_probe;

static int rshim_usb_product_ids[] = {
  USB_BLUEFIELD_1_PRODUCT_ID,
  USB_BLUEFIELD_2_PRODUCT_ID
};

static void rshim_usb_delete(rshim_backend_t *bd)
{
  rshim_usb_t *dev = container_of(bd, rshim_usb_t, bd);

  rshim_deregister(bd);
  RSHIM_INFO("rshim %s deleted\n", bd->dev_name);
  if (dev->handle) {
    libusb_close(dev->handle);
    dev->handle = NULL;
  }
  free(dev);
}

/* Rshim read/write routines */

static int rshim_usb_read_rshim(rshim_backend_t *bd, int chan, int addr,
                                uint64_t *result)
{
  rshim_usb_t *dev = container_of(bd, rshim_usb_t, bd);
  int rc;

  if (!bd->has_rshim)
    return -ENODEV;

  /* Do a blocking control read and endian conversion. */
  rc = libusb_control_transfer(dev->handle,
                               LIBUSB_ENDPOINT_IN |
                               LIBUSB_REQUEST_TYPE_VENDOR |
                               LIBUSB_RECIPIENT_ENDPOINT,
                               0, chan, addr,
                               (unsigned char *)&dev->ctrl_data,
                               sizeof(dev->ctrl_data),
                               RSHIM_USB_TIMEOUT);

  /*
   * The RShim HW puts bytes on the wire in little-endian order
   * regardless of endianness settings either in the host or the ARM
   * cores.
   */
  *result = le64toh(dev->ctrl_data);
  if (rc == sizeof(dev->ctrl_data))
    return 0;

  /*
   * These are weird error codes, but we want to use something
   * the USB stack doesn't use so that we can identify short/long
   * reads.
   */
  return rc >= 0 ? (rc > sizeof(dev->ctrl_data) ? -EINVAL : -ENXIO) : rc;
}

static int rshim_usb_write_rshim(rshim_backend_t *bd, int chan, int addr,
                                 uint64_t value)
{
  rshim_usb_t *dev = container_of(bd, rshim_usb_t, bd);
  int rc;

  if (!bd->has_rshim)
    return -ENODEV;

  /* Convert the word to little endian and do blocking control write. */
  dev->ctrl_data = htole64(value);
  rc = libusb_control_transfer(dev->handle,
                               LIBUSB_ENDPOINT_OUT |
                               LIBUSB_REQUEST_TYPE_VENDOR |
                               LIBUSB_RECIPIENT_ENDPOINT,
                               0, chan, addr,
                               (unsigned char *)&dev->ctrl_data,
                               sizeof(dev->ctrl_data),
                               RSHIM_USB_TIMEOUT);

  if (rc == sizeof(dev->ctrl_data))
    return 0;

  /*
   * These are weird error codes, but we want to use something
   * the USB stack doesn't use so that we can identify short/long
   * writes.
   */
  return rc >= 0 ? (rc > sizeof(dev->ctrl_data) ? -EINVAL : -ENXIO) : rc;
}

/* Boot routines */

static ssize_t rshim_usb_boot_write(rshim_usb_t *dev, const char *buf,
                                    size_t count)
{
  int transferred;
  int rc;

  rc = libusb_bulk_transfer(dev->handle,
                            dev->boot_fifo_ep,
                            (void *)buf, count,
                            &transferred, RSHIM_USB_TIMEOUT);

  if (!rc || rc == LIBUSB_ERROR_TIMEOUT)
    return transferred;
  else
    return rc;
}

/* FIFO routines */

static void rshim_usb_fifo_read_callback(struct libusb_transfer *urb)
{
  rshim_usb_t *dev = urb->user_data;
  rshim_backend_t *bd = &dev->bd;

  RSHIM_DBG("fifo_read_callback: %s urb completed, status %d, "
            "actual length %d, intr buf 0x%x\n",
            dev->read_urb_is_intr ? "interrupt" : "read",
            urb->status, urb->actual_length, (int)*dev->intr_buf);

  pthread_mutex_lock(&bd->ringlock);

  bd->spin_flags &= ~RSH_SFLG_READING;

  switch (urb->status) {
  case LIBUSB_TRANSFER_COMPLETED:
    /*
     * If a read completed, clear the number of bytes available
     * from the last interrupt, and set up the new buffer for
     * processing.  (If an interrupt completed, there's nothing
     * to do, since the number of bytes available was already
     * set by the I/O itself.)
     */
    if (!dev->read_urb_is_intr) {
      *dev->intr_buf = 0;
      bd->read_buf_bytes = urb->actual_length;
      bd->read_buf_next = 0;
    }

    /* Process any data we got, and launch another I/O if needed. */
    rshim_notify(bd, RSH_EVENT_FIFO_INPUT, 0);
    break;

  case LIBUSB_TRANSFER_NO_DEVICE:
    /*
     * The urb was explicitly cancelled.  The only time we
     * currently do this is when we close the stream.  If we
     * mark this as an error, tile-monitor --resume won't work,
     * so we just want to do nothing.
     */
    break;

  case LIBUSB_TRANSFER_TIMED_OUT:
  case LIBUSB_TRANSFER_STALL:
  case LIBUSB_TRANSFER_OVERFLOW:
    if (dev->read_or_intr_retries < READ_RETRIES && urb->actual_length == 0) {
      /*
       * We got an error which could benefit from being retried.
       * Just submit the same urb again.  Note that we don't
       * handle partial reads; it's hard, and we haven't really
       * seen them.
       */
      int rc;

      dev->read_or_intr_retries++;
      rc = libusb_submit_transfer(urb);
      if (rc) {
        RSHIM_DBG("fifo_read_callback: resubmitted urb but got error %d\n", rc);
        /*
         * In this case, we won't try again; signal the
         * error to upper layers.
         */
        rshim_notify(bd, RSH_EVENT_FIFO_ERR, rc > 0 ? -rc : rc);
      } else {
        bd->spin_flags |= RSH_SFLG_READING;
      }
      break;
    }

  case LIBUSB_TRANSFER_CANCELLED:
    break;

  default:
    /*
     * We got some error we don't know how to handle, or we got
     * too many errors.  Either way we don't retry any more,
     * but we signal the error to upper layers.
     */
    RSHIM_DBG("fifo_read_callback: %s urb completed abnormally, "
              "error %d\n", dev->read_urb_is_intr ? "interrupt" : "read",
              urb->status);
    rshim_notify(bd, RSH_EVENT_FIFO_ERR,
                 urb->status > 0 ? -urb->status : urb->status);
    break;
  }

  pthread_mutex_unlock(&bd->ringlock);
}

static void rshim_usb_fifo_read(rshim_usb_t *dev, char *buffer, size_t count)
{
  rshim_backend_t *bd = &dev->bd;
  struct libusb_transfer *urb;
  int rc;

  if (!bd->has_rshim || !bd->has_tm || bd->drop_mode)
    return;

  if ((int) *dev->intr_buf || bd->read_buf_bytes) {
    /* We're doing a read. */
    urb = dev->read_or_intr_urb;

    libusb_fill_bulk_transfer(urb, dev->handle, dev->tm_fifo_in_ep,
                              (uint8_t *)buffer, count,
                              rshim_usb_fifo_read_callback,
                              dev, RSHIM_USB_TIMEOUT);

    dev->bd.spin_flags |= RSH_SFLG_READING;
    dev->read_urb_is_intr = 0;
    dev->read_or_intr_retries = 0;

    /* Submit the urb. */
    rc = libusb_submit_transfer(urb);
    if (rc) {
      dev->bd.spin_flags &= ~RSH_SFLG_READING;
      RSHIM_ERR("usb_fifo_read: failed to submit read urb, error %d\n", rc);
    }
    RSHIM_DBG("usb_fifo_read: submit read urb\n");
  } else {
    /* We're doing an interrupt. */
    urb = dev->read_or_intr_urb;

    libusb_fill_interrupt_transfer(urb, dev->handle,
                                   dev->tm_fifo_int_ep,
                                   (unsigned char *)dev->intr_buf,
                                   sizeof(*dev->intr_buf),
                                   rshim_usb_fifo_read_callback,
                                   dev,
#ifdef __linux__
                                   -1
#else
                                   0
#endif
                                   );

    dev->bd.spin_flags |= RSH_SFLG_READING;
    dev->read_urb_is_intr = 1;
    dev->read_or_intr_retries = 0;

    /* Submit the urb */
    rc = libusb_submit_transfer(urb);
    if (rc) {
      dev->bd.spin_flags &= ~RSH_SFLG_READING;
      RSHIM_DBG("usb_fifo_read: failed submitting interrupt urb %d\n", rc);
    }
    RSHIM_DBG("usb_fifo_read: submit interrupt urb\n");
  }
}

static void rshim_usb_fifo_write_callback(struct libusb_transfer *urb)
{
  rshim_usb_t *dev = urb->user_data;
  rshim_backend_t *bd = &dev->bd;

  RSHIM_DBG("usb_fifo_write_callback: urb completed, status %d, "
            "actual length %d, intr buf %d\n",
            urb->status, urb->actual_length, (int) *dev->intr_buf);

  pthread_mutex_lock(&bd->ringlock);

  bd->spin_flags &= ~RSH_SFLG_WRITING;

  switch (urb->status) {
  case LIBUSB_TRANSFER_COMPLETED:
    /* A write completed. */
    pthread_cond_broadcast(&bd->fifo_write_complete_cond);
    rshim_notify(bd, RSH_EVENT_FIFO_OUTPUT, 0);
    break;

  case LIBUSB_TRANSFER_NO_DEVICE:
    /*
     * The urb was explicitly cancelled.  The only time we
     * currently do this is when we close the stream.  If we
     * mark this as an error, tile-monitor --resume won't work,
     * so we just want to do nothing.
     */
    break;

  case LIBUSB_TRANSFER_TIMED_OUT:
  case LIBUSB_TRANSFER_STALL:
  case LIBUSB_TRANSFER_OVERFLOW:
    if (dev->write_retries < WRITE_RETRIES && urb->actual_length == 0) {
      /*
       * We got an error which could benefit from being retried.
       * Just submit the same urb again.  Note that we don't
       * handle partial writes; it's hard, and we haven't really
       * seen them.
       */
      int rc;

      dev->write_retries++;
      rc = libusb_submit_transfer(urb);
      if (rc) {
        RSHIM_ERR("usb_fifo_write_callback: resubmitted urb but "
                  "got error %d\n", rc);
        /*
         * In this case, we won't try again; signal the
         * error to upper layers.
         */
        rshim_notify(bd, RSH_EVENT_FIFO_ERR, rc > 0 ? -rc : rc);
      } else {
        bd->spin_flags |= RSH_SFLG_WRITING;
      }
      break;
    }

  case LIBUSB_TRANSFER_CANCELLED:
    break;

  default:
    /*
     * We got some error we don't know how to handle, or we got
     * too many errors.  Either way we don't retry any more,
     * but we signal the error to upper layers.
     */
    RSHIM_ERR("usb_fifo_write_callback: urb completed abnormally %d\n",
              urb->status);
    rshim_notify(bd, RSH_EVENT_FIFO_ERR,
                 urb->status > 0 ? -urb->status : urb->status);
    break;
  }

  pthread_mutex_unlock(&bd->ringlock);
}

static int rshim_usb_fifo_write(rshim_usb_t *dev, const char *buffer,
                                size_t count)
{
  rshim_backend_t *bd = &dev->bd;
  int rc;

  if (!bd->has_rshim || !bd->has_tm)
    return -ENODEV;

  if (bd->drop_mode)
    return 0;

  if (count % 8)
    RSHIM_WARN("rshim write %d is not multiple of 8 bytes\n", (int)count);

  /* Initialize the urb properly. */
  libusb_fill_bulk_transfer(dev->write_urb,  dev->handle,
                            dev->tm_fifo_out_ep, (uint8_t *)buffer,
                            count, rshim_usb_fifo_write_callback,
                            dev, RSHIM_USB_TIMEOUT);
  dev->write_retries = 0;

  /* Send the data out the bulk port. */
  rc = libusb_submit_transfer(dev->write_urb);
  if (rc) {
    bd->spin_flags &= ~RSH_SFLG_WRITING;
    RSHIM_DBG("usb_fifo_write: failed submitting write urb, error %d\n", rc);
    return -1;
  }

  bd->spin_flags |= RSH_SFLG_WRITING;
  return 0;
}

/* Probe routines */

/* These make the endpoint test code in rshim_usb_probe() a lot cleaner. */
#define USB_ENDPOINT_XFERTYPE_MASK      0x03
#define USB_ENDPOINT_DIR_MASK           0x80
#define is_in_ep(ep)   (((ep)->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == \
      LIBUSB_ENDPOINT_IN)
#define is_bulk_ep(ep) (((ep)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == \
      LIBUSB_TRANSFER_TYPE_BULK)
#define is_int_ep(ep)  (((ep)->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == \
      LIBUSB_TRANSFER_TYPE_INTERRUPT)
#define max_pkt(ep)    le16_to_cpu(ep->wMaxPacketSize)
#define ep_addr(ep)    (ep->bEndpointAddress)

static ssize_t rshim_usb_backend_read(rshim_backend_t *bd, int devtype,
                                      char *buf, size_t count)
{
  rshim_usb_t *dev = container_of(bd, rshim_usb_t, bd);

  switch (devtype) {
  case RSH_DEV_TYPE_TMFIFO:
    rshim_usb_fifo_read(dev, buf, count);
    return 0;

  default:
    RSHIM_ERR("bad devtype %d\n", devtype);
    return -EINVAL;
  }
}

static ssize_t rshim_usb_backend_write(rshim_backend_t *bd, int devtype,
                                       const char *buf, size_t count)
{
  rshim_usb_t *dev = container_of(bd, rshim_usb_t, bd);

  switch (devtype) {
  case RSH_DEV_TYPE_TMFIFO:
    return rshim_usb_fifo_write(dev, buf, count);

  case RSH_DEV_TYPE_BOOT:
    return rshim_usb_boot_write(dev, buf, count);

  default:
    RSHIM_ERR("bad devtype %d\n", devtype);
    return -EINVAL;
  }
}

static void rshim_usb_backend_cancel_req(rshim_backend_t *bd, int devtype,
                                         bool is_write)
{
  rshim_usb_t *dev = container_of(bd, rshim_usb_t, bd);

  switch (devtype) {
  case RSH_DEV_TYPE_TMFIFO:
    if (is_write)
      libusb_cancel_transfer(dev->write_urb);
    else
      libusb_cancel_transfer(dev->read_or_intr_urb);
    break;

  default:
    RSHIM_ERR("bad devtype %d\n", devtype);
    break;
  }
}

static int rshim_usb_probe_one(libusb_context *ctx, libusb_device *usb_dev,
                               struct libusb_device_descriptor *desc)
{
  int i, rc;
  const struct libusb_interface_descriptor *iface_desc;
  const struct libusb_endpoint_descriptor *ep;
  const struct libusb_interface *interface;
  struct libusb_config_descriptor *config;
  char dev_name[RSHIM_DEV_NAME_LEN], *p;
  uint8_t port_numbers[8] = {0}, bus;
  libusb_device_handle *handle;
  rshim_usb_t *dev = NULL;
  rshim_backend_t *bd;

  /* Check if already exists. */
  rshim_lock();
  bd = rshim_find_by_dev(usb_dev);
  rshim_unlock();
  if (bd)
    return 0;

  /* Check bus number and the port path of the rshim device path. */
  bus = libusb_get_bus_number(usb_dev);
#if HAVE_LIBUSB_GET_PORT_NUMBERS || defined(__FreeBSD__)
  rc = libusb_get_port_numbers(usb_dev, port_numbers, sizeof(port_numbers));
#elif HAVE_LIBUSB_GET_DEVICE_ADDRESS
  port_numbers[0] = libusb_get_device_address(usb_dev);
  rc = 1;
#else
  rc = -EINVAL;
#endif
  if (rc <= 0) {
    perror("Failed to get USB ports\n");
    return -ENODEV;
  }
  sprintf(dev_name, "usb-%x", bus);
  p = dev_name + strlen(dev_name);
  for (i = 0; i < rc; i++) {
    sprintf(p, "%c%x", (i == rc - 1) ? '.' : '-', port_numbers[i]);
    p += strlen(p);
  }

  if (!rshim_allow_device(dev_name))
    return -EACCES;

  RSHIM_INFO("Probing %s\n", dev_name);

  rc = libusb_get_active_config_descriptor(usb_dev, &config);
  if (rc) {
    perror("Failed to get active config\n");
    return -ENODEV;
  }

  rc = libusb_open(usb_dev, &handle);
  if (rc) {
    perror("Failed to open USB device: %m\n");
    return rc;
  }

  for (i = 0; i < config->bNumInterfaces; i++) {
    rc = libusb_claim_interface(handle, i);
    if (rc < 0) {
      if (libusb_kernel_driver_active(handle, i) == 1) {
        perror("Kernel driver is running. Please uninstall it first.\n");
        exit(rc);
      } else {
        perror("Failed to claim interface\n");
      }
      libusb_close(handle);
      return rc;
    }
  }

  /*
   * Now see if we've previously seen this device.  If so, we use the
   * same device number, otherwise we pick the first available one.
   */
  rshim_lock();

  /* Find the backend. */
  bd = rshim_find_by_name(dev_name);
  if (bd) {
    RSHIM_INFO("found %s\n", dev_name);
    dev = container_of(bd, rshim_usb_t, bd);
  } else {
    RSHIM_INFO("create rshim %s\n", dev_name);
    dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
      RSHIM_ERR("couldn't get memory for new device");
      goto error;
    }

    bd = &dev->bd;
    strcpy(bd->dev_name, dev_name);
    bd->read = rshim_usb_backend_read;
    bd->write = rshim_usb_backend_write;
    bd->cancel = rshim_usb_backend_cancel_req;
    bd->destroy = rshim_usb_delete;
    bd->read_rshim = rshim_usb_read_rshim;
    bd->write_rshim = rshim_usb_write_rshim;
    bd->has_reprobe = 1;
    pthread_mutex_init(&bd->mutex, NULL);
  }

  rshim_ref(bd);
  bd->dev = usb_dev;
  dev->handle = handle;
  switch (desc->idProduct) {
    case USB_BLUEFIELD_2_PRODUCT_ID:
      bd->ver_id = RSHIM_BLUEFIELD_2;
      break;
    default:
      bd->ver_id = RSHIM_BLUEFIELD_1;
  }
  bd->rev_id = desc->bcdDevice;

  if (!dev->intr_buf) {
    dev->intr_buf = calloc(1, sizeof(*dev->intr_buf));
    if (dev->intr_buf != NULL)
      *dev->intr_buf = 0;
  }

  if (!dev->read_or_intr_urb)
    dev->read_or_intr_urb = libusb_alloc_transfer(0);

  if (!dev->write_urb)
    dev->write_urb = libusb_alloc_transfer(0);

  if (!dev->read_or_intr_urb || !dev->write_urb) {
    RSHIM_ERR("can't allocate buffers or urbs\n");
    goto error;
  }

  pthread_mutex_lock(&bd->mutex);

  for (i = 0; i < config->bNumInterfaces; i++) {
    interface = &config->interface[i];
    if (interface->num_altsetting <= 0)
      continue;
    iface_desc = &interface->altsetting[0];

    if (iface_desc->bInterfaceSubClass == 0) {
      RSHIM_DBG("Found rshim interface\n");

      /*
       * We only expect one endpoint here, just make sure its
       * attributes match.
       */
      if (iface_desc->bNumEndpoints != 1) {
        RSHIM_ERR("wrong number of endpoints for rshim interface\n");
        pthread_mutex_unlock(&bd->mutex);
        goto error;
      }
      ep = &iface_desc->endpoint[0];

      /* We expect a bulk out endpoint. */
      if (!is_bulk_ep(ep) || is_in_ep(ep)) {
        pthread_mutex_unlock(&bd->mutex);
        goto error;
      }

      bd->has_rshim = 1;
      dev->boot_fifo_ep = ep_addr(ep);
    } else if (iface_desc->bInterfaceSubClass == 1) {
      RSHIM_DBG("Found tmfifo interface\n");
      /*
       * We expect 3 endpoints here.  Since they're listed in
       * random order we have to use their attributes to figure
       * out which is which.
       */
      if (iface_desc->bNumEndpoints != 3) {
        RSHIM_ERR("wrong number of endpoints for tm interface\n");
        pthread_mutex_unlock(&bd->mutex);
        goto error;
      }
      dev->tm_fifo_in_ep = 0;
      dev->tm_fifo_int_ep = 0;
      dev->tm_fifo_out_ep = 0;

      for (i = 0; i < iface_desc->bNumEndpoints; i++) {
        ep = &iface_desc->endpoint[i];

        if (is_in_ep(ep)) {
          if (is_bulk_ep(ep)) {
            /* Bulk in endpoint. */
            dev->tm_fifo_in_ep = ep_addr(ep);
          } else if (is_int_ep(ep)) {
            /* Interrupt in endpoint. */
            dev->tm_fifo_int_ep = ep_addr(ep);
          }
        } else {
          if (is_bulk_ep(ep)) {
            /* Bulk out endpoint. */
            dev->tm_fifo_out_ep = ep_addr(ep);
          }
        }
      }

      if (!dev->tm_fifo_in_ep || !dev->tm_fifo_int_ep ||
          !dev->tm_fifo_out_ep) {
        RSHIM_ERR("could not find all required endpoints for tm interface\n");
        pthread_mutex_unlock(&bd->mutex);
        goto error;
      }
      bd->has_tm = 1;
    } else {
      pthread_mutex_unlock(&bd->mutex);
      goto error;
    }
  }

  /*
   * Register rshim here since it needs to detect whether other backend
   * has already registered or not, which involves reading/writting rshim
   * registers and has assumption that the under layer is working.
   */
  rc = rshim_register(bd);
  if (rc) {
    pthread_mutex_unlock(&bd->mutex);
    goto error;
  }

  /* Notify that device is attached. */
  rc = rshim_notify(bd, RSH_EVENT_ATTACH, 0);
  pthread_mutex_unlock(&bd->mutex);
  if (rc)
    goto error;

  rshim_unlock();
  return 0;

error:
  if (dev) {
    libusb_free_transfer(dev->read_or_intr_urb);
    dev->read_or_intr_urb = NULL;

    libusb_free_transfer(dev->write_urb);
    dev->write_urb = NULL;

    free(dev->intr_buf);
    dev->intr_buf = NULL;

    rshim_deref(bd);
  }

  rshim_unlock();
  return rc;
}

static void rshim_usb_disconnect(struct libusb_device *usb_dev)
{
  rshim_backend_t *bd;
  rshim_usb_t *dev;

  rshim_lock();

  bd = rshim_find_by_dev(usb_dev);
  if (!bd) {
    rshim_unlock();
    return;
  }

  dev = container_of(bd, rshim_usb_t, bd);

  rshim_notify(bd, RSH_EVENT_DETACH, 0);

  /*
   * Clear this interface so we don't unregister our devices next
   * time.
   */
  pthread_mutex_lock(&bd->mutex);

  bd->has_rshim = 0;

  /*
   * We must make sure the console worker isn't running
   * before we free all these resources, and particularly
   * before we decrement our usage count, below.  Most of the
   * time, if it's even enabled, it'll be scheduled to run at
   * some point in the future, and we can take care of that
   * by asking that it be canceled.
   *
   * However, it's possible that it's already started
   * running, but can't make progress because it's waiting
   * for the device mutex, which we currently have.  We
   * handle this case by clearing the bit that says it's
   * enabled.  The worker tests this bit as soon as it gets
   * the mutex, and if it's clear, it just returns without
   * rescheduling itself.  Note that if we didn't
   * successfully cancel it, we flush the work entry below,
   * after we drop the mutex, to be sure it's done before we
   * decrement the device usage count.
   *
   * XXX This might be racy; what if something else which
   * would enable the worker runs after we drop the mutex
   * but before the worker itself runs?
   */
  bd->has_cons_work = 0;

  libusb_cancel_transfer(dev->read_or_intr_urb);
  dev->read_or_intr_urb = NULL;
  libusb_cancel_transfer(dev->write_urb);
  dev->write_urb = NULL;

  free(dev->intr_buf);
  dev->intr_buf = NULL;

  if (!bd->has_rshim && !bd->has_tm)
    RSHIM_INFO("USB disconnected\n");
  else
    RSHIM_INFO("USB partially disconnected\n");

  pthread_mutex_unlock(&bd->mutex);

  if (dev->handle) {
    libusb_close(dev->handle);
    dev->handle = NULL;
  }

  rshim_deref(bd);
  rshim_unlock();
}

static int rshim_usb_add_poll(libusb_context *ctx)
{
  const struct libusb_pollfd **usb_pollfd = libusb_get_pollfds(ctx);
  struct epoll_event event;
  int i = 0, rc = -ENODEV;

  if (!usb_pollfd)
    return rc;

  memset(&event, 0, sizeof(event));

  while (usb_pollfd[i]) {
    event.data.fd = usb_pollfd[i]->fd;
    event.events = 0;

#define	RSHIM_CONVERT(flag) do { \
  if (usb_pollfd[i]->events & flag) \
    event.events |= E##flag; \
} while (0)

  RSHIM_CONVERT(POLLIN);
  RSHIM_CONVERT(POLLOUT);
#ifdef __linux__
  RSHIM_CONVERT(POLLRDNORM);
  RSHIM_CONVERT(POLLRDBAND);
  RSHIM_CONVERT(POLLWRNORM);
  RSHIM_CONVERT(POLLWRBAND);
  RSHIM_CONVERT(POLLWRBAND);
#endif
  RSHIM_CONVERT(POLLERR);
  RSHIM_CONVERT(POLLHUP);

#undef RSHIM_CONVERT

    rc = epoll_ctl(rshim_usb_epoll_fd, EPOLL_CTL_ADD, usb_pollfd[i]->fd,
                   &event);
    if (rc == -1 && errno != EEXIST)
      RSHIM_ERR("epoll_ctl failed; %m\n");
    i++;
  }

  free(usb_pollfd);

  return rc;
}

#if LIBUSB_API_VERSION >= 0x01000102
static libusb_hotplug_callback_handle rshim_hotplug_handle;

static int rshim_hotplug_callback(struct libusb_context *ctx,
                                  struct libusb_device *dev,
                                  libusb_hotplug_event event,
                                  void *user_data)
{
  switch (event) {
  case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
    /*
     * The probe function would send control packet which could cause race
     * condition when calling from the hotplug callback function. Thus set
     * a flag and do it later in the main loop.
     */
    RSHIM_INFO("USB device detected\n");
    rshim_usb_need_probe = true;
    break;

  case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
    RSHIM_INFO("USB device leaving\n");
    rshim_usb_disconnect(dev);
    break;

  default:
    break;
  }

  rshim_usb_add_poll(ctx);

  return 0;	/* keep filter registered */
}
#endif

static bool rshim_usb_probe(void)
{
  libusb_context *ctx = rshim_usb_ctx;
  libusb_device **devs, *dev;
  int rc, i = 0, j, num;

  rc = libusb_get_device_list(ctx, &devs);
  if (rc < 0) {
    perror("USB Get Device Error\n");
    return false;
  }

  while ((dev = devs[i++]) != NULL) {
    struct libusb_device_descriptor desc;

    rc = libusb_get_device_descriptor(dev, &desc);
    if (rc)
      continue;

    if (desc.idVendor != USB_TILERA_VENDOR_ID)
      continue;

    num = sizeof(rshim_usb_product_ids) / sizeof(rshim_usb_product_ids[0]);
    for (j = 0; j < num; j++) {
      if (desc.idProduct == rshim_usb_product_ids[j])
        rshim_usb_probe_one(ctx, dev, &desc);
    }
  }

  rc = rshim_usb_add_poll(ctx);
  if (rc)
    return false;

  return true;
}

int rshim_usb_init(int epoll_fd)
{
  libusb_context *ctx = NULL;
  int rc, i, num;

  rc = libusb_init(&ctx);
  if (rc < 0) {
    RSHIM_ERR("USB Init Error: %m\n");
    return rc;
  }

#ifdef LIBUSB_LOG_LEVEL_ERROR
  if (rshim_log_level > LOG_ERR)
    libusb_set_debug(ctx, LIBUSB_LOG_LEVEL_ERROR);
#endif

  rshim_usb_ctx = ctx;
  rshim_usb_epoll_fd = epoll_fd;

#if LIBUSB_API_VERSION >= 0x01000102
  num = sizeof(rshim_usb_product_ids) / sizeof(rshim_usb_product_ids[0]);
  for (i = 0; i < num; i++) {
    /*
     * Register ARRIVED and LEFT separately to avoid the coverity 'mixed enum'
     * warnings.
     */
    rc = libusb_hotplug_register_callback(ctx,
                                          LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
                                          LIBUSB_HOTPLUG_ENUMERATE,
                                          USB_TILERA_VENDOR_ID,
                                          rshim_usb_product_ids[i],
                                          LIBUSB_HOTPLUG_MATCH_ANY,
                                          rshim_hotplug_callback,
                                          NULL,
                                          &rshim_hotplug_handle);
    if (rc != LIBUSB_SUCCESS) {
      RSHIM_ERR("failed to register hotplug callback\n");
      libusb_exit(ctx);
      rshim_usb_ctx = NULL;
      return rc;
    }

    rc = libusb_hotplug_register_callback(ctx,
                                          LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
                                          LIBUSB_HOTPLUG_ENUMERATE,
                                          USB_TILERA_VENDOR_ID,
                                          rshim_usb_product_ids[i],
                                          LIBUSB_HOTPLUG_MATCH_ANY,
                                          rshim_hotplug_callback,
                                          NULL,
                                          &rshim_hotplug_handle);
    if (rc != LIBUSB_SUCCESS) {
      RSHIM_ERR("failed to register hotplug callback\n");
      libusb_exit(ctx);
      rshim_usb_ctx = NULL;
      return rc;
    }
  }
#else
  rshim_usb_probe();
#endif

  return 0;
}

void rshim_usb_poll(void)
{
  struct timeval tv = {0, 0};

  if (!rshim_usb_ctx)
    return;

  if (rshim_usb_need_probe) {
    rshim_usb_need_probe = false;
    rshim_usb_probe();
  }

  libusb_handle_events_timeout_completed(rshim_usb_ctx, &tv, NULL);
}
