/*
  xsns_87_mcp2515.ino - MCP2515 CAN bus support for Tasmota

  Copyright (C) 2021  Marius Bezuidenhout

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_SPI
#ifdef USE_MCP2515
/*********************************************************************************************\
 * MCP2515 - Microchip CAN controller
 *
 * Connections:
 * MCP2515  ESP8266         Tasmota
 * -------  --------------  ----------
 *  INT     not used
 *  SCK     GPIO14          SPI CLK
 *  SI      GPIO13          SPI MOSI
 *  SO      GPIO12          SPI MISO
 *  CS      GPIO0..5,15,16  MCP2515
 *  Gnd     Gnd
 *  VCC     Vin/5V
\*********************************************************************************************/

#define XSNS_87              87

// set defaults if not defined
#ifndef MCP2515_BITRATE
  #define MCP2515_BITRATE  CAN_500KBPS
#endif

#ifndef MCP2515_CLOCK
  #define MCP2515_CLOCK  MCP_8MHZ
#endif

#ifndef MCP2515_MAX_FRAMES
  #define MCP2515_MAX_FRAMES 14
#endif

#ifndef CAN_KEEP_ALIVE_SECS
  #define CAN_KEEP_ALIVE_SECS 300
#endif

#ifndef MCP2515_TIMEOUT
  #define MCP2515_TIMEOUT 10
#endif

#ifndef MCP2515_BMS_CLIENT
  #define MCP2515_BMS_CLIENT
  // Look for Freedom Won BMS data in CAN message
  #ifndef MCP2515_BMS_FREEDWON
    #define MCP2515_BMS_FREEDWON
  #endif  // MCP2515_BMS_FREEDWON
#endif // MCP2515_BMS_CLIENT

#include "mcp2515.h"

#ifdef MCP2515_BMS_CLIENT

  #define BMS_NAME                0x1
  #define BMS_SOC                 0x2
  #define BMS_SOH                 0x4
  #define BMS_CHARGE_VOLT_MAX     0x8
  #define BMS_CHARGE_VOLT_MIN    0x10
  #define BMS_CHARGE_AMP_MAX     0x20
  #define BMS_CHARGE_AMP_MIN     0x40
  #define BMS_VOLT               0x80
  #define BMS_AMP               0x100
  #define BMS_TEMP              0x200
  #define BMS_CAPACITY          0x400
  #define BMS_MODEL             0x800
  #define BMS_FIRMWARE_VER     0x1000
  #define BMS_MANUFACTURER     0x2000
  #define BMS_SERIAL           0x4000

struct BMS_Struct {
  uint16_t  setFields; // Bitwise fields set list
  char      name[17];
  uint16_t  stateOfCharge;
  uint16_t  stateOfHealth;
  uint16_t  chargeVoltLimit; // Div 10
  uint16_t  dischargeVolt; // Div 10
  int16_t   maxChargeCurrent; // Div 10
  int16_t   maxDischargeCurrent; // Div 10
  uint16_t  battVoltage; // Div 100
  int16_t   battAmp; // Div 10
  int16_t   battTemp; // Div 10
  uint16_t  capacityAh;
  uint16_t  model;
  uint16_t  firmwareVer;
  char      manuf[9];
  char      serialNr[17];
} bms;

#endif

int8_t mcp2515_init_status = 1;

uint32_t lastFrameRecv = 0;
struct can_frame canFrame;
MCP2515 *mcp2515 = nullptr;

char c2h(char c)
{
  return "0123456789ABCDEF"[0x0F & (unsigned char)c];
}

void MCP2515_FrameSizeError(uint8_t len, uint32_t id) {
  AddLog(LOG_LEVEL_DEBUG, PSTR("MCP2515: Unexpected length (%d) for ID 0x%x"), len, id);
}

void MCP2515_Init(void) {
  mcp2515 = new MCP2515(5);
  if (MCP2515::ERROR_OK != mcp2515->reset()) {
    AddLog(LOG_LEVEL_ERROR, PSTR("MCP2515: Failed to reset module"));
    mcp2515_init_status = 0;
  }

  if (MCP2515::ERROR_OK != mcp2515->setBitrate(MCP2515_BITRATE, MCP2515_CLOCK)) {
    AddLog(LOG_LEVEL_ERROR, PSTR("MCP2515: Failed to set module bitrate"));
    mcp2515_init_status = 0;
  }
  
  if (mcp2515_init_status && MCP2515::ERROR_OK != mcp2515->setNormalMode()) {
    AddLog(LOG_LEVEL_ERROR, PSTR("MCP2515: Failed to set normal mode"));
    mcp2515_init_status = 0;
  }
#ifdef MCP2515_BMS_FREEDWON
  // TODO: Filter CAN bus messages
  //mcp2515->setFilterMask();
  //mcp2515->setFilter();
#endif
}

void MCP2515_Read() {
  uint8_t nCounter = 0;
  bool checkRcv;
  if (mcp2515_init_status) {

    checkRcv = mcp2515->checkReceive();

    while (checkRcv && nCounter <= MCP2515_MAX_FRAMES) {
      mcp2515->checkReceive();
      nCounter++;
      if (mcp2515->readMessage(&canFrame) == MCP2515::ERROR_OK) {
        lastFrameRecv = TasmotaGlobal.uptime;
#ifdef MCP2515_BMS_CLIENT
  #ifdef MCP2515_BMS_FREEDWON
      switch (canFrame.can_id) {
        // Charge/Discharge parameters
        case 0x351:
          if (8 == canFrame.can_dlc) {
            bms.chargeVoltLimit     = (canFrame.data[1] << 8) | canFrame.data[0];
            bms.maxChargeCurrent    = (canFrame.data[3] << 8) | canFrame.data[2];
            bms.maxDischargeCurrent = (canFrame.data[5] << 8) | canFrame.data[4];
            bms.dischargeVolt       = (canFrame.data[7] << 8) | canFrame.data[6];
            bms.setFields          |= BMS_CHARGE_VOLT_MAX | BMS_CHARGE_VOLT_MIN | BMS_CHARGE_AMP_MAX | BMS_CHARGE_AMP_MIN;
          } else {
            MCP2515_FrameSizeError(canFrame.can_dlc, canFrame.can_id);
          }
          break;
        // State of Charge/Health
        case 0x355:
          if (6 >= canFrame.can_dlc) {
            bms.stateOfCharge = (canFrame.data[1] << 8) | canFrame.data[0];
            bms.stateOfHealth = (canFrame.data[3] << 8) | canFrame.data[2];
            bms.setFields    |= BMS_SOC | BMS_SOH;
          } else {
            MCP2515_FrameSizeError(canFrame.can_dlc, canFrame.can_id);
          }
          break;
        // Voltage/Current/Temperature
        case 0x356:
          if (6 >= canFrame.can_dlc) {
            bms.battVoltage = (canFrame.data[1] << 8) | canFrame.data[0];
            bms.battAmp     = (canFrame.data[3] << 8) | canFrame.data[2];
            bms.battTemp    = (canFrame.data[5] << 8) | canFrame.data[4]; // Convert to fahrenheit if SetOpion8 is set
            bms.setFields  |= BMS_VOLT | BMS_AMP | BMS_TEMP;
          } else {
            MCP2515_FrameSizeError(canFrame.can_dlc, canFrame.can_id);
          }
          break;
        // Manufacturer name
        case 0x35E:
          for (int i = 0; i < canFrame.can_dlc; i++) {
            bms.manuf[i] = canFrame.data[i];
          }
          bms.setFields |= BMS_MANUFACTURER;
          bms.manuf[8] = 0; // Ensure that name is null terminated
          break;
        // Battery Model / Firmware version
        case 0x35F:
          if (4 == canFrame.can_dlc) {
            bms.model       = (canFrame.data[1] << 8) | canFrame.data[0];
            bms.firmwareVer = (canFrame.data[3] << 8) | canFrame.data[2];
            bms.setFields  |= BMS_MODEL | BMS_FIRMWARE_VER;
          } else {
            MCP2515_FrameSizeError(canFrame.can_dlc, canFrame.can_id);
          }
          break;
        // Battery / BMS name
        case 0x370:
        case 0x371:
          for (int i = 0; i < canFrame.can_dlc; i++) {
            uint8_t nameStrPos = i + ((canFrame.can_id & 0x1) * 8); // If can_id is 0x371 then fill from byte 8 onwards
            bms.name[nameStrPos] = canFrame.data[i];
          }
          if ((canFrame.can_id & 0x1) && (bms.name[0] > 0)) { // Upper and lower part of name has been set now
            bms.setFields  |= BMS_NAME;
          }
          bms.name[16] = 0; // Ensure that name is null terminated
          break;
        // Modules status
        case 0x372:
        // Min/Max cell voltage/temperature
        case 0x373:
        // Min. cell voltage id string
        case 0x374:
        // Max. cell voltage id string
        case 0x375:
        // Min. cell temperature id string
        case 0x376:
        // Max. cell temperature id string
        case 0x377:
          break;
        // Installed capacity
        case 0x379:
          if (2 >= canFrame.can_dlc) {
            bms.capacityAh = (canFrame.data[1] << 8) | canFrame.data[0];
            bms.setFields |= BMS_CAPACITY;
          } else {
            MCP2515_FrameSizeError(canFrame.can_dlc, canFrame.can_id);
          }
          break;
        // Serial number
        case 0x380:
        case 0x381:
          for (int i = 0; i < canFrame.can_dlc; i++) {
            uint8_t serialNrStrPos = i + ((canFrame.can_id & 0x1) * 8); // If can_id is 0x381 then fill from byte 8 onwards
            bms.serialNr[serialNrStrPos] = canFrame.data[i];
          }
          if ((canFrame.can_id & 0x1) && (bms.serialNr[0] > 0)) { // Upper and lower part of serial number has been set now
            bms.setFields  |= BMS_SERIAL;
          }
          bms.serialNr[16] = 0; // Ensure that serial nr is null terminated
          break;
        default:
          char canMsg[17];
          canMsg[0] = 0;
          for (int i = 0; i < canFrame.can_dlc; i++) {
            canMsg[i*2] = c2h(canFrame.data[i]>>4);
            canMsg[i*2+1] = c2h(canFrame.data[i]);
          }
          if (canFrame.can_dlc > 0) {
            canMsg[(canFrame.can_dlc - 1) * 2 + 2] = 0;
          }
          AddLog(LOG_LEVEL_DEBUG, PSTR("MCP2515: Received message 0x%s from ID 0x%x"), canMsg, (uint32_t)canFrame.can_id);
          break;
      }
  #endif // MCP2515_BMS_FREEDWON
#endif // MCP2515_BMS_CLIENT
      } else if (mcp2515->checkError()) {
        uint8_t errFlags = mcp2515->getErrorFlags();
          mcp2515->clearRXnOVRFlags();
          AddLog(LOG_LEVEL_DEBUG, PSTR("MCP2515: Received error %d"), errFlags);
          break;
      }
    }

#ifdef MCP2515_BMS_FREEDWON
    if (!(TasmotaGlobal.uptime%CAN_KEEP_ALIVE_SECS) && TasmotaGlobal.uptime>60) {
      canFrame.can_id  = 0x305;
      canFrame.can_dlc = 0;
      if (MCP2515::ERROR_OK != mcp2515->sendMessage(&canFrame)) {
        AddLog(LOG_LEVEL_ERROR, PSTR("MCP2515: Failed to send keep alive frame"));
      }
    }
#endif
  }
}

void MCP2515_Show(bool Json) {
  if (Json) {
    if (lastFrameRecv > 0 && TasmotaGlobal.uptime - lastFrameRecv <= MCP2515_TIMEOUT) {
#ifdef MCP2515_BMS_CLIENT
      if (bms.setFields & BMS_MANUFACTURER) {
        bool jsonFirstField = true;
        ResponseAppend_P(PSTR(",\"%s\":{"), bms.manuf);

        if (bms.setFields & BMS_SOC) {
          ResponseAppend_P(PSTR("\"SOC\":%d"), bms.stateOfCharge);
          jsonFirstField = false;
        }
        if (bms.setFields & BMS_SOH) {
          ResponseAppend_P(PSTR("%s\"SOH\":%d"), jsonFirstField ? PSTR("") : PSTR(","), bms.stateOfHealth);
          jsonFirstField = false;
        }
        if (bms.setFields & BMS_VOLT) {
          ResponseAppend_P(PSTR("%s\"BattVolt\":%d.%d"), jsonFirstField ? PSTR("") : PSTR(","), bms.battVoltage / 100, bms.battVoltage % 100);
          jsonFirstField = false;
        }
        if (bms.setFields & BMS_AMP) {
          int8_t bmsModNeg = 1;
          if (bms.battAmp < 0) {
            bmsModNeg = -1;
          }
          ResponseAppend_P(PSTR("%s\"BattAmp\":%d.%d"), jsonFirstField ? PSTR("") : PSTR(","), bms.battAmp / 10, (bms.battAmp % 10) * bmsModNeg);
          jsonFirstField = false;
        }
        if (bms.setFields & BMS_TEMP) {
          ResponseAppend_P(PSTR("%s\"BattTemp\":%d.%d"), jsonFirstField ? PSTR("") : PSTR(","), bms.battTemp / 10, bms.battTemp % 10);
          jsonFirstField = false;
        }
        ResponseAppend_P(PSTR("}"));
      }
    } else {
      bms.setFields = 0;
#endif // MCP2515_BMS_CLIENT
    }
#ifdef USE_WEBSERVER
  } else {
  #ifdef MCP2515_BMS_CLIENT
    if (bms.setFields & BMS_MANUFACTURER) {
      if (bms.setFields & BMS_SOC) {
        WSContentSend_PD(HTTP_SNS_SOC, bms.manuf, bms.stateOfCharge);
      }
      if (bms.setFields & BMS_SOH) {
        WSContentSend_PD(HTTP_SNS_SOH, bms.manuf, bms.stateOfHealth);
      }
      if (bms.setFields & BMS_VOLT) {
        WSContentSend_Voltage(bms.manuf, (float(bms.battVoltage) / 100));
      }
      if (bms.setFields & BMS_AMP) {
        char ampStr[6];
        dtostrf((float(bms.battAmp) / 10), 5, 1, ampStr);
        WSContentSend_PD(PSTR("{s}%s " D_CURRENT "{m}%s " D_UNIT_AMPERE "{e}"), bms.manuf, ampStr);
      }
      if (bms.setFields & BMS_TEMP) {
        WSContentSend_Temp(bms.manuf, ConvertTemp(float(bms.battTemp) / 10));
      }
      if (bms.setFields & BMS_CHARGE_VOLT_MAX) {
        char voltStr[6];
        dtostrf((float(bms.chargeVoltLimit) / 10), 5, 1, voltStr);
        WSContentSend_PD(PSTR("{s}%s Max Voltage{m}%s " D_UNIT_AMPERE "{e}"), bms.manuf, voltStr);
      }
      if (bms.setFields & BMS_CHARGE_VOLT_MIN) {
        char voltStr[6];
        dtostrf((float(bms.dischargeVolt) / 10), 5, 1, voltStr);
        WSContentSend_PD(PSTR("{s}%s Min Voltage{m}%s " D_UNIT_AMPERE "{e}"), bms.manuf, voltStr);
      }
      if (bms.setFields & BMS_CHARGE_AMP_MAX) {
        char ampStr[6];
        dtostrf((float(bms.maxChargeCurrent) / 10), 5, 1, ampStr);
        WSContentSend_PD(PSTR("{s}%s Max Charge Current{m}%s " D_UNIT_AMPERE "{e}"), bms.manuf, ampStr);
      }
      if (bms.setFields & BMS_CHARGE_AMP_MIN) {
        char ampStr[6];
        dtostrf((float(bms.maxDischargeCurrent) / 10), 5, 1, ampStr);
        WSContentSend_PD(PSTR("{s}%s Max Discharge Current{m}%s " D_UNIT_AMPERE "{e}"), bms.manuf, ampStr);
      }

    } else {
      WSContentSend_PD(PSTR("{s}MCP2515 {m} Waiting for data{e}"));
    }
  #endif // MCP2515_BMS_CLIENT
#endif  // USE_WEBSERVER
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xsns87(uint8_t function)
{
  bool result = false;

  if (PinUsed(GPIO_MCP2515_CS, GPIO_ANY) && TasmotaGlobal.spi_enabled) {
    switch (function) {
      case FUNC_INIT:
        MCP2515_Init();
        break;
      case FUNC_EVERY_50_MSECOND:
        MCP2515_Read();
        break;
      case FUNC_JSON_APPEND:
        MCP2515_Show(1);
        break;

#ifdef USE_WEBSERVER
      case FUNC_WEB_SENSOR:
        MCP2515_Show(0);
        break;
#endif  // USE_WEBSERVER
    }
  }
  return result;
}

#endif  // USE_MCP2515
#endif // USE_SPI