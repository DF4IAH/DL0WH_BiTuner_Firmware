/*
 * device_adc.c
 *
 *  Created on: 06.01.2019
 *      Author: DF4IAH
 */

#include "stm32l4xx_hal.h"
#include "stm32l4xx_it.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"

#include "main.h"
#include "device_adc.h"


extern ADC_HandleTypeDef    hadc1;

extern EventGroupHandle_t   adcEventGroupHandle;


static uint16_t             s_adc1_dma_buf[ADC1_DMA_CHANNELS] = { 0U };

static _Bool                s_adc_isFwd                       = 0;

extern float                g_adc_refint_val;
extern float                g_adc_vref_mv;
extern float                g_adc_bat_mv;
extern float                g_adc_temp_deg;
extern float                g_adc_fwd_mv;
extern float                g_adc_rev_mv;
extern float                g_adc_vdiode_mv;


void adcStartConv(ADC_ENUM_t adc)
{
  switch (adc) {
  case ADC_REFINT_VAL:
  case ADC_BAT_MV:
  case ADC_TEMP_DEG:
  case ADC_VDIODE_MV:
  case ADC_FWD_MV:
    {
      xEventGroupClearBits(adcEventGroupHandle,
          EG_ADC__CONV_AVAIL_REFINT | EG_ADC__CONV_AVAIL_BAT | EG_ADC__CONV_AVAIL_TEMP | EG_ADC__CONV_AVAIL_VDIODE |
          EG_ADC__CONV_AVAIL_FWD);
      HAL_ADC_Stop_DMA(&hadc1);

      /* Switch MUX to FWD input */
      HAL_GPIO_WritePin(GPIO_SWR_SEL_REV_GPIO_Port, GPIO_SWR_SEL_REV_Pin, GPIO_PIN_RESET);
      __DMB();
      HAL_GPIO_WritePin(GPIO_SWR_SEL_FWD_GPIO_Port, GPIO_SWR_SEL_FWD_Pin, GPIO_PIN_SET);
      s_adc_isFwd = true;

      #if 0
      /* Have constant delay for low pass filter in front of the ADC */
      for (uint8_t delay = 255U; delay; --delay)
        ;
      #endif

      HAL_ADC_Start_DMA(&hadc1, (uint32_t*) s_adc1_dma_buf, (sizeof(s_adc1_dma_buf) / sizeof(uint16_t)));
    }
    break;

  case ADC_REV_MV:
    {
      xEventGroupClearBits(adcEventGroupHandle,
          EG_ADC__CONV_AVAIL_REFINT | EG_ADC__CONV_AVAIL_BAT | EG_ADC__CONV_AVAIL_TEMP | EG_ADC__CONV_AVAIL_VDIODE |
          EG_ADC__CONV_AVAIL_REV);
      HAL_ADC_Stop_DMA(&hadc1);

      /* Switch MUX to FWD input */
      HAL_GPIO_WritePin(GPIO_SWR_SEL_FWD_GPIO_Port, GPIO_SWR_SEL_FWD_Pin, GPIO_PIN_RESET);
      __DMB();
      HAL_GPIO_WritePin(GPIO_SWR_SEL_REV_GPIO_Port, GPIO_SWR_SEL_REV_Pin, GPIO_PIN_SET);
      s_adc_isFwd = false;

      #if 0
      /* Have constant delay for low pass filter in front of the ADC */
      for (uint8_t delay = 255U; delay; --delay)
        ;
      #endif

      HAL_ADC_Start_DMA(&hadc1, (uint32_t*) s_adc1_dma_buf, (sizeof(s_adc1_dma_buf) / sizeof(uint16_t)));
    }
    break;

  default:
    { }
  }
}

void adcStopConv(ADC_ENUM_t adc)
{
  switch (adc) {
  case ADC_REFINT_VAL:
  case ADC_BAT_MV:
  case ADC_TEMP_DEG:
  case ADC_VDIODE_MV:
  case ADC_FWD_MV:
  case ADC_REV_MV:
    HAL_ADC_Stop_DMA(&hadc1);
    break;

  default:
    { }
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
  const float c_infiniteResponseWeight  = 0.98f;
  static float s_refintVal              = 23600.0f;    // Thumb value to start with
  static float s_batVal                 = 15310.0f;    // Thumb value to start with
  static float s_tempVal                = 14090.0f;    // Thumb value to start with
  static float s_vdiodeVal              = 12800.0f; // Thumb value to start with
  BaseType_t pxHigherPriorityTaskWoken  = pdFALSE;

  if (hadc == &hadc1) {
    EventBits_t eb = EG_ADC__CONV_AVAIL_REFINT | EG_ADC__CONV_AVAIL_BAT | EG_ADC__CONV_AVAIL_TEMP;

    /* RefInt, Bat, Temp */
    {
      const uint16_t* TS_CAL1_ADDR  = (const uint16_t*) 0x1FFF75A8;
      const uint16_t* TS_CAL2_ADDR  = (const uint16_t*) 0x1FFF75CA;

      /* Infinite response */
      s_refintVal  = (s_refintVal * c_infiniteResponseWeight) + ((1.0f - c_infiniteResponseWeight) * (float)s_adc1_dma_buf[0]);
      s_batVal     = (s_batVal    * c_infiniteResponseWeight) + ((1.0f - c_infiniteResponseWeight) * (float)s_adc1_dma_buf[1]);
      s_tempVal    = (s_tempVal   * c_infiniteResponseWeight) + ((1.0f - c_infiniteResponseWeight) * (float)s_adc1_dma_buf[2]);

      g_adc_refint_val = s_refintVal / 16.0f;
      g_adc_vref_mv    = (((float)VREFINT_CAL_VREF * (*VREFINT_CAL_ADDR)) / g_adc_refint_val) + ADC_V_OFFS_VREF_mV;

      g_adc_bat_mv     = ((ADC_MUL_BAT * s_batVal * g_adc_vref_mv) / 65535.0f)                + ADC_V_OFFS_BAT_mV;

      g_adc_temp_deg   = ((float)(110 - 30) / ((*TS_CAL2_ADDR) - (*TS_CAL1_ADDR))) * ((s_tempVal * ADC_MUL_TEMP / 16.f) - (*TS_CAL1_ADDR)) + 30.f;
    }

    /* FWD vs. REV */
    if (s_adc_isFwd) {
      /* Forward voltage */
      g_adc_fwd_mv = (s_adc1_dma_buf[3] * g_adc_vref_mv / 65535.0f)                         + ADC_V_OFFS_FWDREV_mV;
      eb |= EG_ADC__CONV_AVAIL_FWD;

    } else {
      /* Reverse voltage */
      g_adc_rev_mv = (s_adc1_dma_buf[3] * g_adc_vref_mv / 65535.0f)                         + ADC_V_OFFS_FWDREV_mV;
      eb |= EG_ADC__CONV_AVAIL_REV;
    }

    /* Diode voltage for reference */
    {
      const uint32_t val = s_adc1_dma_buf[4];
      s_vdiodeVal = (s_vdiodeVal * c_infiniteResponseWeight) + ((1.0f - c_infiniteResponseWeight) * (float)val);

      g_adc_vdiode_mv = (s_vdiodeVal * g_adc_vref_mv / 65535.0f)                              + ADC_V_OFFS_VDIODE_mV;
      eb |= EG_ADC__CONV_AVAIL_VDIODE;
    }

    xEventGroupSetBitsFromISR(adcEventGroupHandle, eb, &pxHigherPriorityTaskWoken);
  }
}
