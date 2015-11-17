/************************************************************************************
 * drivers/timers/ds3231.c
 *
 *   Copyright (C) 2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
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
 ************************************************************************************/

/************************************************************************************
 * Included Files
 ************************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/i2c.h>
#include <nuttx/timers/ds3231.h>

#include "ds3231.h"

#ifdef CONFIG_RTC_DS3231

/************************************************************************************
 * Pre-processor Definitions
 ************************************************************************************/
/* Configuration ********************************************************************/
/* This RTC implementation supports only date/time RTC hardware */

#ifndef CONFIG_RTC_DATETIME
#  error CONFIG_RTC_DATETIME must be set to use this driver
#endif

#ifdef CONFIG_RTC_HIRES
#  error CONFIG_RTC_HIRES must NOT be set with this driver
#endif

#ifndef CONFIG_I2C_TRANSFER
#  error CONFIG_I2C_TRANSFER is required by this driver
#endif

#ifndef CONFIG_DS3231_I2C_FREQUENCY
#  error CONFIG_DS3231_I2C_FREQUENCY is not configured
#  define CONFIG_DS3231_I2C_FREQUENCY 400000
#endif

#if CONFIG_DS3231_I2C_FREQUENCY > 400000
#  error CONFIG_DS3231_I2C_FREQUENCY is out of range
#endif

#define DS3231_I2C_ADDRESS 0x68

#ifndef CONFIG_DEBUG
#  undef CONFIG_DEBUG_RTC
#endif

/* Debug ****************************************************************************/

#ifdef CONFIG_DEBUG_RTC
#  define rtcdbg    dbg
#  define rtcvdbg   vdbg
#  define rtclldbg  lldbg
#  define rtcllvdbg llvdbg
#else
#  define rtcdbg(x...)
#  define rtcvdbg(x...)
#  define rtclldbg(x...)
#  define rtcllvdbg(x...)
#endif

/************************************************************************************
 * Priviate Types
 ************************************************************************************/
/* This structure describes the state of the DS3231 chip.  Only a single RTC is
 * supported.
 */

struct ds3231_dev_s
{
  FAR struct i2c_dev_s *i2c;  /* Contained reference to the I2C bus driver */
};

/************************************************************************************
 * Public Data
 ************************************************************************************/

/* g_rtc_enabled is set true after the RTC has successfully initialized */

volatile bool g_rtc_enabled = false;

/************************************************************************************
 * Private Data
 ************************************************************************************/
/* The state of the DS3231 chip.  Only a single RTC is supported */

static struct ds3231_dev_s g_ds3231;

/************************************************************************************
 * Private Functions
 ************************************************************************************/

/************************************************************************************
 * Name: rtc_dumptime
 *
 * Description:
 *   Show the broken out time.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ************************************************************************************/

#ifdef CONFIG_DEBUG_RTC
static void rtc_dumptime(FAR struct tm *tp, FAR const char *msg)
{
  rtclldbg("%s:\n", msg);
  rtclldbg("   tm_sec: %08x\n", tp->tm_sec);
  rtclldbg("   tm_min: %08x\n", tp->tm_min);
  rtclldbg("  tm_hour: %08x\n", tp->tm_hour);
  rtclldbg("  tm_mday: %08x\n", tp->tm_mday);
  rtclldbg("   tm_mon: %08x\n", tp->tm_mon);
  rtclldbg("  tm_year: %08x\n", tp->tm_year);
#if defined(CONFIG_LIBC_LOCALTIME) || defined(CONFIG_TIME_EXTENDED)
  rtclldbg("  tm_wday: %08x\n", tp->tm_wday);
  rtclldbg("  tm_yday: %08x\n", tp->tm_yday);
  rtclldbg(" tm_isdst: %08x\n", tp->tm_isdst);
#endif
}
#else
#  define rtc_dumptime(tp, msg)
#endif

/************************************************************************************
 * Name: rtc_bin2bcd
 *
 * Description:
 *   Converts a 2 digit binary to BCD format
 *
 * Input Parameters:
 *   value - The byte to be converted.
 *
 * Returned Value:
 *   The value in BCD representation
 *
 ************************************************************************************/

static uint8_t rtc_bin2bcd(int value)
{
  uint8_t msbcd = 0;

  while (value >= 10)
    {
      msbcd++;
      value -= 10;
    }

  return (msbcd << 4) | value;
}

/************************************************************************************
 * Name: rtc_bin2bcd
 *
 * Description:
 *   Convert from 2 digit BCD to binary.
 *
 * Input Parameters:
 *   value - The BCD value to be converted.
 *
 * Returned Value:
 *   The value in binary representation
 *
 ************************************************************************************/

static int rtc_bcd2bin(uint8_t value)
{
  int tens = ((int)value >> 4) * 10;
  return tens + (value & 0x0f);
}

/************************************************************************************
 * Public Functions
 ************************************************************************************/

/************************************************************************************
 * Name: ds3231_rtc_initialize
 *
 * Description:
 *   Initialize the hardware RTC per the selected configuration.  This function is
 *   called once during the OS initialization sequence
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int ds3231_rtc_initialize(FAR struct i2c_dev_s *i2c)
{
  /* Remember the i2c device and claim that the RTC is enabled */

  g_ds3231.i2c  = i2c;
  g_rtc_enabled = true;
  return OK;
}

/************************************************************************************
 * Name: up_rtc_getdatetime
 *
 * Description:
 *   Get the current date and time from the date/time RTC.  This interface
 *   is only supported by the date/time RTC hardware implementation.
 *   It is used to replace the system timer.  It is only used by the RTOS during
 *   initialization to set up the system time when CONFIG_RTC and CONFIG_RTC_DATETIME
 *   are selected (and CONFIG_RTC_HIRES is not).
 *
 *   NOTE: Some date/time RTC hardware is capability of sub-second accuracy.  That
 *   sub-second accuracy is lost in this interface.  However, since the system time
 *   is reinitialized on each power-up/reset, there will be no timing inaccuracy in
 *   the long run.
 *
 * Input Parameters:
 *   tp - The location to return the high resolution time value.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_getdatetime(FAR struct tm *tp)
{
  struct i2c_msg_s msg[2];
  uint8_t buffer[8];
  int tmp;
  int ret;

  /* Select to begin reading at the seconds register */

  buffer[0] = DS3231_TIME_SECR;

  msg[0].addr   = DS3231_I2C_ADDRESS;
  msg[0].flags  = 0;
  msg[0].buffer = buffer;
  msg[0].length = 1;

  /* Set up to read 7 registers: secs, min, hr, dow, date, mth, yr */

  msg[1].addr   = DS3231_I2C_ADDRESS;
  msg[1].flags  = I2C_M_READ;
  msg[1].buffer = &buffer[1];
  msg[1].length = 7;

  /* Configure I2C before using it */

  I2C_SETFREQUENCY(g_ds3231.i2c, CONFIG_DS3231_I2C_FREQUENCY);

  /* Perform the transfer (This could be done with I2C_WRITEREAD()) */

  ret = I2C_TRANSFER(g_ds3231.i2c, msg, 1);
  if (ret < 0)
    {
      rtcdbg("ERROR: I2C_TRANSFER failed: %d\n", ret)
      return ret;
    }

  /* Format the return time */
  /* Return seconds (0-61) */

  tp->tm_sec = rtc_bcd2bin(buffer[1] & DS3231_TIME_SEC_BCDMASK);

  /* Return minutes (0-59) */

  tp->tm_min = rtc_bcd2bin(buffer[2] & DS3231_TIME_MIN_BCDMASK);

  /* Return hour (0-23).  This assumes 24-hour time was set. */

  tp->tm_hour = rtc_bcd2bin(buffer[3] & DS3231_TIME_HOUR24_BCDMASK);

 #if defined(CONFIG_LIBC_LOCALTIME) || defined(CONFIG_TIME_EXTENDED)
  /* Return the day of the week (0-6) */

  tp->tm_wday = (rtc_bcd2bin(buffer[4]) & DS3231_TIME_DAY_MASK)- 1;
#endif

  /* Return the day of the month (1-31) */

  tp->tm_mday = rtc_bcd2bin(buffer[5] & DS3231_TIME_DATE_BCDMASK);

  /* Return the month (0-11) */

  tp->tm_mon = rtc_bcd2bin(buffer[6] & DS3231_TIME_MONTH_BCDMASK) - 1;

  /* Return the years since 1990 */

  tmp = rtc_bcd2bin(buffer[7] & DS3231_TIME_YEAR_BCDMASK);

  if ((buffer[6] & DS3231_TIME_CENTURY_MASK) == DS3231_TIME_1900)
    {
      tp->tm_year = tmp;
    }
  else
    {
      tp->tm_year = tmp + 100;
    }

  rtc_dumptime(tp, "Returning");
  return OK;
}

/************************************************************************************
 * Name: up_rtc_settime
 *
 * Description:
 *   Set the RTC to the provided time.  All RTC implementations must be able to
 *   set their time based on a standard timespec.
 *
 * Input Parameters:
 *   tp - the time to use
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure
 *
 ************************************************************************************/

int up_rtc_settime(FAR const struct timespec *tp)
{
  struct i2c_msg_s msg;
  struct tm newtm;
  time_t newtime;
  uint8_t buffer[8];
  uint8_t century;
  uint8_t year;
  int ret;

  rtc_dumptime(tp, "Setting time");

  /* Get the broken out time */

  newtime = (time_t)tp->tv_sec;
  if (tp->tv_nsec >= 500000000)
    {
      /* Round up */

      newtime++;
    }

 #ifdef CONFIG_LIBC_LOCALTIME
   if (localtime_r(&newtime, &newtm) == NULL)
     {
       rtcdbg("ERROR: localtime_r failed\n")
       return -EINVAL;
     }
#else
   if (gmtime_r(&newtime, &newtm) == NULL)
     {
       rtcdbg("ERROR: gmtime_r failed\n")
       return -EINVAL;
     }
#endif
  
  rtc_dumptime(&tm, "New time");

  /* Construct the message */
  /* Write starting with the seconds regiser */

  buffer[0] = DS3231_TIME_SECR;

  /* Save seconds (0-59) converted to BCD */

  buffer[1] = rtc_bin2bcd(newtm.tm_sec);

  /* Save minutes (0-59) converted to BCD */

  buffer[2] = rtc_bin2bcd(newtm.tm_min);

  /* Save hour (0-23) with 24-hour time indicatin */

  buffer[3] = rtc_bin2bcd(newtm.tm_hour) | DS3231_TIME_24;

  /* Save the day of the week (1-7) */

#if defined(CONFIG_LIBC_LOCALTIME) || defined(CONFIG_TIME_EXTENDED)
  buffer[4] = rtc_bin2bcd(newtm.tm_wday + 1);
#else
  buffer[4] = 1;
#endif

  /* Save the day of the week (1-31) */

  buffer[5] = rtc_bin2bcd(newtm.tm_mday);

  /* Handle years in the 20th vs the 21st century */

  if (newtm.tm_year < 100)
    {
      /* Convert years in the range 1900-1999 */

      century = DS3231_TIME_1900;
      year    = newtm.tm_year;
    }
  else
    {
      /* Convert years in the range 2000-2099 */

      century = DS3231_TIME_2000;
      year    = newtm.tm_year - 100;
    }

  /* Save the month (1-12) with century */

  buffer[6] = rtc_bin2bcd(newtm.tm_mon + 1) | century;

  /* Save the year */

  buffer[7] = year;

  /* Setup the I2C message */

  msg.addr   = DS3231_I2C_ADDRESS;
  msg.flags  = 0;
  msg.buffer = buffer;
  msg.length = 8;

  /* Configure I2C before using it */

  I2C_SETFREQUENCY(g_ds3231.i2c, CONFIG_DS3231_I2C_FREQUENCY);

  /* Perform the transfer (This could be done with I2C_READ) */

  ret = I2C_TRANSFER(g_ds3231.i2c, &msg, 1);
  if (ret < 0)
    {
      rtcdbg("ERROR: I2C_TRANSFER failed: %d\n", ret)
    }

  return ret;
}

#endif /* CONFIG_RTC_DS3231 */
