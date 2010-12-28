/*******************************************************************************
 * drivers/usbhost/usbhost_enumerate.c
 *
 *   Copyright (C) 2010 Gregory Nutt. All rights reserved.
 *   Authors: Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

/*******************************************************************************
 * Included Files
 *******************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/usb/ohci.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/usbhost.h>

/*******************************************************************************
 * Definitions
 *******************************************************************************/

/*******************************************************************************
 * Private Types
 *******************************************************************************/

/*******************************************************************************
 * Private Function Prototypes
 *******************************************************************************/

static inline uint16_t usbhost_getle16(const uint8_t *val);
static void usbhost_putle16(uint8_t *dest, uint16_t val);

static inline int usbhost_devdesc(const struct usb_devdesc_s *devdesc, int desclen,
                                  struct usbhost_id_s *id);
static inline int usbhost_configdesc(const uint8_t *configdesc, int desclen,
                                     struct usbhost_id_s *id);
static inline int usbhost_classbind(FAR struct usbhost_driver_s *drvr,
                                    const uint8_t *configdesc, int desclen,
                                    struct usbhost_id_s *id,
                                    FAR struct usbhost_class_s **class);

/*******************************************************************************
 * Private Data
 *******************************************************************************/

/*******************************************************************************
 * Public Data
 *******************************************************************************/

/*******************************************************************************
 * Private Functions
 *******************************************************************************/

/****************************************************************************
 * Name: usbhost_getle16
 *
 * Description:
 *   Get a (possibly unaligned) 16-bit little endian value.
 *
 *******************************************************************************/

static inline uint16_t usbhost_getle16(const uint8_t *val)
{
  return (uint16_t)val[1] << 8 | (uint16_t)val[0];
}

/****************************************************************************
 * Name: usbhost_putle16
 *
 * Description:
 *   Put a (possibly unaligned) 16-bit little endian value.
 *
 *******************************************************************************/

static void usbhost_putle16(uint8_t *dest, uint16_t val)
{
  dest[0] = val & 0xff; /* Little endian means LS byte first in byte stream */
  dest[1] = val >> 8;
}

/*******************************************************************************
 * Name: usbhost_devdesc
 *
 * Description:
 *   A configuration descriptor has been obtained from the device.  Find the
 *   ID information for the class that supports this device.
 *
 *******************************************************************************/

static inline int usbhost_devdesc(const struct usb_devdesc_s *devdesc,
                                  int desclen, struct usbhost_id_s *id)
{
  /* Clear the ID info */

  memset(id, 0, sizeof(struct usbhost_id_s));

  /* Check we have enough of the structure to see the ID info. */

  if (desclen >= 7)
    {
      /* Pick off the ID info */

      id->base     = devdesc->class;
      id->subclass = devdesc->subclass;
      id->proto    = devdesc->protocol;

      /* Check if we have enough of the structure to see the VID/PID */

      if (desclen >= 12)
        {
          /* Yes, then pick off the VID and PID as well */

          id->vid = usbhost_getle16(devdesc->vendor);
          id->pid = usbhost_getle16(devdesc->product);
        }
    }

  return OK;
}
                                
/*******************************************************************************
 * Name: usbhost_configdesc
 *
 * Description:
 *   A configuration descriptor has been obtained from the device.  Find the
 *   ID information for the class that supports this device.
 *
 *******************************************************************************/

static inline int usbhost_configdesc(const uint8_t *configdesc, int desclen,
                                     struct usbhost_id_s *id)
{
  struct usb_cfgdesc_s *cfgdesc;
  struct usb_ifdesc_s *ifdesc;
  int remaining;

  DEBUGASSERT(configdesc != NULL &&
              desclen >= sizeof(struct usb_cfgdesc_s));
  
  /* Verify that we were passed a configuration descriptor */

  cfgdesc = (struct usb_cfgdesc_s *)configdesc;
  if (cfgdesc->type != USB_DESC_TYPE_CONFIG)
    {
      return -EINVAL;
    }

  /* Get the total length of the configuration descriptor (little endian).
   * It might be a good check to get the number of interfaces here too.
  */

  remaining = (int)usbhost_getle16(cfgdesc->totallen);

  /* Skip to the next entry descriptor */

  configdesc += cfgdesc->len;
  remaining  -= cfgdesc->len;

  /* Loop where there are more dscriptors to examine */

  memset(&id, 0, sizeof(FAR struct usb_desc_s));
  while (remaining >= sizeof(struct usb_desc_s))
    {
      /* What is the next descriptor? Is it an interface descriptor? */

      ifdesc = (struct usb_ifdesc_s *)configdesc;
      if (ifdesc->type == USB_DESC_TYPE_INTERFACE)
        {
          /* Yes, extract the class information from the interface descriptor.
           * (We are going to need to do more than this here in the future:
           *  ID information might lie elsewhere and we will need the VID and
           *  PID as well).
           */
 
          DEBUGASSERT(remaining >= sizeof(struct usb_ifdesc_s));
          id->base     = ifdesc->class;
          id->subclass = ifdesc->subclass;
          id->proto    = ifdesc->protocol;
          return OK;
        }

     /* Increment the address of the next descriptor */
 
      configdesc += ifdesc->len;
      remaining  -= ifdesc->len;
    }

  return -ENOENT;
}

/*******************************************************************************
 * Name: usbhost_classbind
 *
 * Description:
 *   A configuration descriptor has been obtained from the device.  Try to
 *   bind this configuration descriptor with a supported class.
 *
 *******************************************************************************/

static inline int usbhost_classbind(FAR struct usbhost_driver_s *drvr,
                                    const uint8_t *configdesc, int desclen,
                                    struct usbhost_id_s *id,
                                    FAR struct usbhost_class_s **class)
{
  FAR struct usbhost_class_s *devclass;
  const struct usbhost_registry_s *reg;
  int ret = -EINVAL;

  if (id->base == USB_CLASS_VENDOR_SPEC)
    {
      udbg("BUG: More logic needed to extract VID and PID\n");
    }

  /* Is there is a class implementation registered to support this device. */

  reg = usbhost_findclass(id);
  uvdbg("usbhost_findclass: %p\n", reg);
  if (reg)
    {
      /* Yes.. there is a class for this device.  Get an instance of
       * its interface.
       */

      ret = -ENOMEM;
      devclass = CLASS_CREATE(reg, drvr, id);
      uvdbg("CLASS_CREATE: %p\n", devclass);
      if (devclass)
        {
          /* Then bind the newly instantiated class instance */

          ret = CLASS_CONNECT(devclass, configdesc, desclen);
          if (ret != OK)
            {
              udbg("CLASS_CONNECT failed: %d\n", ret);
              CLASS_DISCONNECTED(devclass);
            }
          else
            {
              *class = devclass;
            }
        }
    }

  uvdbg("Returning: %d\n", ret);
  return ret;
}

/*******************************************************************************
 * Public Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: usbhost_enumerate
 *
 * Description:
 *   Enumerate the connected device.  As part of this enumeration process,
 *   the driver will (1) get the device's configuration descriptor, (2)
 *   extract the class ID info from the configuration descriptor, (3) call
 *   usbhost_findclass() to find the class that supports this device, (4)
 *   call the create() method on the struct usbhost_registry_s interface
 *   to get a class instance, and finally (5) call the configdesc() method
 *   of the struct usbhost_class_s interface.  After that, the class is in
 *   charge of the sequence of operations.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

int usbhost_enumerate(FAR struct usbhost_driver_s *drvr,
                      FAR struct usbhost_class_s **class)
{
  struct usb_ctrlreq_s *ctrlreq;
  struct usbhost_id_s id;
  size_t maxlen;
  unsigned int len;
  uint16_t maxpacketsize;
  uint8_t *buffer;
  int  ret;

  DEBUGASSERT(drvr && class);

  /* Allocate TD buffers for use in this function.  We will need two:
   * One for the request and one for the data buffer.
   */

  ret = DRVR_ALLOC(drvr, (FAR uint8_t **)&ctrlreq, &maxlen);
  if (ret != OK)
    {
      udbg("DRVR_ALLOC failed: %d\n", ret);
      return ret;
    }

  ret = DRVR_ALLOC(drvr, &buffer, &maxlen);
  if (ret != OK)
    {
      udbg("DRVR_ALLOC failed: %d\n", ret);
      goto errout;
    }

  /* Set max pkt size = 8 */

  DRVR_EP0CONFIGURE(drvr, 0, 8);

  /* Read first 8 bytes of the device descriptor */

  ctrlreq->type = USB_REQ_DIR_IN|USB_REQ_RECIPIENT_DEVICE;
  ctrlreq->req  = USB_REQ_GETDESCRIPTOR;
  usbhost_putle16(ctrlreq->value, (USB_DESC_TYPE_DEVICE << 8));
  usbhost_putle16(ctrlreq->index, 0);
  usbhost_putle16(ctrlreq->len, 8);

  ret = DRVR_CTRLIN(drvr, ctrlreq, buffer);
  if (ret != OK)
    {
      udbg("ERROR: GETDESCRIPTOR/DEVICE, DRVR_CTRLIN returned %d\n", ret);
      goto errout;
    }

  /* Extract info from the device descriptor */

  {
    struct usb_devdesc_s *devdesc = (struct usb_devdesc_s *)buffer;

    /* Extract the max packetsize for endpoint 0 */

    DRVR_EP0CONFIGURE(drvr, 0, maxpacketsize);

    /* Get class identification information from the device descriptor.  Most
     * devices set this to USB_CLASS_PER_INTERFACE (zero) and provide the
     * identification informatino in the interface descriptor(s).  That allows
     * a device to support multiple, different classes.
     */

    (void)usbhost_devdesc(devdesc, 8, &id);

    /* NOTE: Additional logic is needed here.  We will need additional logic
     * to (1) get the full device descriptor, (1) extract the vendor/product IDs
     * and (2) extract the number of configurations from the (full) device
     * descriptor.
     */
  }

  /* Set the device address to 1 */

  ctrlreq->type = USB_REQ_DIR_OUT|USB_REQ_RECIPIENT_DEVICE;
  ctrlreq->req  = USB_REQ_SETADDRESS;
  usbhost_putle16(ctrlreq->value, (1 << 8));
  usbhost_putle16(ctrlreq->index, 0);
  usbhost_putle16(ctrlreq->len, 0);

  ret = DRVR_CTRLOUT(drvr, ctrlreq, NULL);
  if (ret != OK)
    {
      udbg("ERROR: SETADDRESS DRVR_CTRLOUT returned %d\n", ret);
      goto errout;
    }
  up_mdelay(2);

  /* Modify control pipe with function address 1 */

  DRVR_EP0CONFIGURE(drvr, 1, maxpacketsize);

 /* Get the configuration descriptor (only), index == 0.  More logic is
  * needed in order to handle devices with multiple configurations.
  */

  ctrlreq->type = USB_REQ_DIR_IN|USB_REQ_RECIPIENT_DEVICE;
  ctrlreq->req  = USB_REQ_GETDESCRIPTOR;
  usbhost_putle16(ctrlreq->value, (USB_DESC_TYPE_CONFIG << 8));
  usbhost_putle16(ctrlreq->index, 0);
  usbhost_putle16(ctrlreq->len, USB_SIZEOF_CFGDESC);

  ret = DRVR_CTRLIN(drvr, ctrlreq, buffer);
  if (ret != OK)
   {
      udbg("ERROR: GETDESCRIPTOR/CONFIG, DRVR_CTRLIN returned %d\n", ret);
      goto errout;
    }

  /* Extract the full size of the configuration data */

  len = ((struct usb_cfgdesc_s *)buffer)->len;

  /* Get all of the configuration descriptor data, index == 0 */

  ctrlreq->type = USB_REQ_DIR_IN|USB_REQ_RECIPIENT_DEVICE;
  ctrlreq->req  = USB_REQ_GETDESCRIPTOR;
  usbhost_putle16(ctrlreq->value, (USB_DESC_TYPE_CONFIG << 8));
  usbhost_putle16(ctrlreq->index, 0);
  usbhost_putle16(ctrlreq->len, len);

  ret = DRVR_CTRLIN(drvr, ctrlreq, buffer);
  if (ret != OK)
    {
      udbg("ERROR: GETDESCRIPTOR/CONFIG, DRVR_CTRLIN returned %d\n", ret);
      goto errout;
    }

  /* Select device configuration 1 */

  ctrlreq->type = USB_REQ_DIR_OUT|USB_REQ_RECIPIENT_DEVICE;
  ctrlreq->req  = USB_REQ_SETCONFIGURATION;
  usbhost_putle16(ctrlreq->value, 1);
  usbhost_putle16(ctrlreq->index, 0);
  usbhost_putle16(ctrlreq->len, 0);

  ret = DRVR_CTRLOUT(drvr, ctrlreq, NULL);
  if (ret != OK)
    {
      udbg("ERROR: SETCONFIGURATION, DRVR_CTRLOUT returned %d\n", ret);
      goto errout;
    }

  /* Free the TD that we were using for the request buffer.  It is not needed
   * further here but it may be needed by the class driver during its connection
   * operations.
   */
 
  DRVR_FREE(drvr, (uint8_t*)ctrlreq);
  ctrlreq = NULL;

  /* Was the class identification information provided in the device descriptor?
   * Or do we need to find it in the interface descriptor(s)?
   */

  if (id.base == USB_CLASS_PER_INTERFACE)
    {
      /* Get the class identification information for this device from the
       * interface descriptor(s).  Hmmm.. More logic is need to handle the
       * case of multiple interface descriptors.
       */

      ret = usbhost_configdesc(buffer, len, &id);
      if (ret != OK)
        {
          udbg("ERROR: usbhost_configdesc returned %d\n", ret);
          goto errout;
        }
    }

  /* Some devices may require this delay before initialization */

  up_mdelay(100);

  /* Parse the configuration descriptor and bind to the class instance for the
   * device.  This needs to be the last thing done because the class driver
   * will begin configuring the device.
   */

  ret = usbhost_classbind(drvr, buffer, len, &id, class);
  if (ret != OK)
    {
      udbg("ERROR: usbhost_classbind returned %d\n", ret);
    }

errout:
  if (buffer)
    {
      DRVR_FREE(drvr, buffer);
    }

  if (ctrlreq)
    {
      DRVR_FREE(drvr, (uint8_t*)ctrlreq);
    }
  return ret;
}
