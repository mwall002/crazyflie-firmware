/**
 *    ||          ____  _ __
 * +------+      / __ )(_) /_______________ _____  ___
 * | 0xBC |     / __  / / __/ ___/ ___/ __ `/_  / / _ \
 * +------+    / /_/ / / /_/ /__/ /  / /_/ / / /_/  __/
 *  ||  ||    /_____/_/\__/\___/_/   \__,_/ /___/\___/
 *
 * Crazyflie control firmware
 *
 * Copyright (C) 2015 Bitcraze AB
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, in version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * deck_ow.c - Functions to decode the decks oneWire memory content
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define DEBUG_MODULE "DECK_INFO"

#include "deck.h"

#include "ow.h"
#include "crc.h"
#include "debug.h"

static int count = 0;
static DeckInfo deckInfos[DECK_MAX_COUNT];

static void enumerateDecks(void);

void deckInfoInit()
{
  static bool isInit = false;

  if (isInit) return;

  enumerateDecks();

  isInit = true;
}

int deckCount(void)
{
  return count;
}

DeckInfo * deckInfo(int i)
{
  if (i<count) {
    return &deckInfos[i];
  }

  return NULL;
}

// Dummy driver for decks that do not have a driver implemented
static const DeckDriver dummyDriver;

static const DeckDriver * findDriver(DeckInfo *deck)
{
  char name[30];
  const DeckDriver *driver = &dummyDriver;

  deckTlvGetString(&deck->tlv, DECK_INFO_NAME, name, 30);

  if (deck->vid) {
    driver = deckFindDriverByVidPid(deck->vid, deck->pid);
  } else if (strlen(name)>0) {
    driver = deckFindDriverByName(name);
  }

  if (driver == NULL)
    driver = &dummyDriver;

  return driver;
}

void printDeckInfo(DeckInfo *info)
{
  char name[30] = "NoName";
  char rev[10] = "NoRev";

  if (deckTlvHasElement(&info->tlv, DECK_INFO_NAME)) {
    deckTlvGetString(&info->tlv, DECK_INFO_NAME, name, 30);
  }

  if (deckTlvHasElement(&info->tlv, DECK_INFO_REVISION)) {
    deckTlvGetString(&info->tlv, DECK_INFO_REVISION, rev, 10);
  }

  DEBUG_PRINT("Deck %02x:%02x %s (Rev. %s)\n", info->vid, info->pid, name, rev);
  DEBUG_PRINT("Used pin: %08x\n", (unsigned int)info->usedPins);

  if (info->driver == &dummyDriver) {
    DEBUG_PRINT("Warning! No driver found for deck.\n");
  } else {
    DEBUG_PRINT("Driver implements: [ %s%s]\n",
                 info->driver->init?"init ":"", info->driver->test?"test ":"");
  }
}

static bool infoDecode(DeckInfo * info)
{
  uint8_t crcHeader;
  uint8_t crcTlv;

  if (info->header != DECK_INFO_HEADER_ID) {
    DEBUG_PRINT("Memory error: wrong header ID\n");
    return false;
  }

  crcHeader = crcSlow(info->raw, DECK_INFO_HEADER_SIZE);
  if(info->crc != crcHeader) {
    DEBUG_PRINT("Memory error: incorrect header CRC\n");
    return false;
  }

  if(info->raw[DECK_INFO_TLV_VERSION_POS] != DECK_INFO_TLV_VERSION) {
    DEBUG_PRINT("Memory error: incorrect TLV version\n");
    return false;
  }

  crcTlv = crcSlow(&info->raw[DECK_INFO_TLV_VERSION_POS], info->raw[DECK_INFO_TLV_LENGTH_POS]+2);
  if(crcTlv != info->raw[DECK_INFO_TLV_DATA_POS + info->raw[DECK_INFO_TLV_LENGTH_POS]]) {
    DEBUG_PRINT("Memory error: incorrect TLV CRC %x!=%x\n", (unsigned int)crcTlv,
                info->raw[DECK_INFO_TLV_DATA_POS + info->raw[DECK_INFO_TLV_LENGTH_POS]]);
    return false;
  }

  info->tlv.data = &info->raw[DECK_INFO_TLV_DATA_POS];
  info->tlv.length = info->raw[DECK_INFO_TLV_LENGTH_POS];

  return true;
}

static void enumerateDecks(void)
{
  uint8_t nDecks = 0;
  int i;
  bool noError = true;
  uint32_t usedPeriph = 0;
  uint32_t usedGpio = 0;

  owInit();

  if (owScan(&nDecks))
  {
    DEBUG_PRINT("Found %d deck memor%s.\n", nDecks, nDecks>1?"ies":"y");
  } else {
    DEBUG_PRINT("Error scanning for deck memories, "
                "no deck drivers will be initialised\n");
    nDecks = 0;
  }

  for (i = 0; i < nDecks; i++)
  {
    DEBUG_PRINT("Enumerating deck %i\n", i);
    if (owRead(i, 0, sizeof(deckInfos[0].raw), (uint8_t *)&deckInfos[i]))
    {
      if (infoDecode(&deckInfos[i]))
      {
        deckInfos[i].driver = findDriver(&deckInfos[i]);

        printDeckInfo(&deckInfos[i]);

        // Check for Periph and Gpio conflict
        if (usedPeriph & deckInfos[i].driver->usedPeriph) {
          DEBUG_PRINT("ERROR: Driver Periph usage conflicts with a "
                      "previously enumerated deck driver. No decks will be "
                      "initialized!\n");
          noError = false;
        }

        if (usedGpio & deckInfos[i].driver->usedGpio) {
          DEBUG_PRINT("ERROR: Driver Gpio usage conflicts with a "
                      "previously enumerated deck driver. No decks will be "
                      "initialized!\n");
          noError = false;
        }

        usedPeriph |= deckInfos[i].driver->usedPeriph;
        usedGpio |= deckInfos[i].driver->usedGpio;
      } else {
        DEBUG_PRINT("Deck %i has corrupted OW memory. "
                    "No driver will be initialized!\n", i);
        noError = false;
      }
    }
    else
    {
      DEBUG_PRINT("Reading deck nr:%d [FAILED]. "
                  "No driver will be initialized!\n", i);
      noError = false;
    }
  }

  if (noError) {
    count = nDecks;
  }

  return;
}

/****** Key/value area handling ********/
static int findType(TlvArea *tlv, int type) {
  int pos = 0;

  while (pos < tlv->length) {
    if (tlv->data[pos] == type) {
      return pos;
    } else {
      pos += tlv->data[pos+1]+2;
    }
  }
  return -1;
}

bool deckTlvHasElement(TlvArea *tlv, int type) {
  return findType(tlv, type) >= 0;
}

int deckTlvGetString(TlvArea *tlv, int type, char *string, int length) {
  int pos = findType(tlv, type);
  int strlength = 0;

  if (pos >= 0) {
    strlength = tlv->data[pos+1];

    if (strlength > (length-1)) {
      strlength = length-1;
    }

    memcpy(string, &tlv->data[pos+2], strlength);
    string[strlength] = '\0';

    return strlength;
  } else {
    string[0] = '\0';

    return -1;
  }
}

char* deckTlvGetBuffer(TlvArea *tlv, int type, int *length) {
  int pos = findType(tlv, type);
  if (pos >= 0) {
    *length = tlv->data[pos+1];
    return (char*) &tlv->data[pos+2];
  }

  return NULL;
}

void deckTlvGetTlv(TlvArea *tlv, int type, TlvArea *output) {
  output->length = 0;
  output->data = (uint8_t *)deckTlvGetBuffer(tlv, type, &output->length);
}
