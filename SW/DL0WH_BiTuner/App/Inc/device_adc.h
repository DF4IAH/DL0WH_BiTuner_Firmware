/*
 * device_adc.h
 *
 *  Created on: 06.01.2019
 *      Author: DF4IAH
 */

#ifndef DEVICE_ADC_H_
#define DEVICE_ADC_H_


#define ADC1_DMA_CHANNELS               3
#define ADC2_DMA_CHANNELS               1


#define LOGAMP_MUL_POT_VAL             68U

#ifdef Lot_PBAF69__Waf_20__Pos_69_24
/* Special settings */

# define ADC_V_OFFS_VREF_mV          -135.4f
# define ADC_V_OFFS_BAT_mV             56.0f
# define ADC_V_OFFS_FWDREV_mV          47.0f
# define ADC_V_OFFS_VDIODE_mV          56.0f

# define ADC_MUL_BAT                    5.74f
# define ADC_MUL_TEMP                   1.657f

# define CMAD_30DEG_ADC_MV            688.0f
# define CMAD_ADC_MVpDEG               -3.13f

# define LOGAMP_OFS_MVpDEG           -102.66f
# define LOGAMP_OFS_POT_VAL           137
# define LOGAMP_OFS_MVpPOTVAL        -100.00f

# else

/* Default settings */
# define ADC_V_OFFS_VREF_mV          -135.4f
# define ADC_V_OFFS_BAT_mV             56.0f
# define ADC_V_OFFS_FWDREV_mV          47.0f
# define ADC_V_OFFS_VDIODE_mV          56.0f

# define ADC_MUL_BAT                    5.24f
# define ADC_MUL_TEMP                   1.50f

#endif


typedef enum ADC_ENUM {

  ADC_ADC1_REFINT_VAL               = 0x00,
  ADC_ADC1_VREF_MV,
  ADC_ADC1_BAT_MV,
  ADC_ADC1_TEMP_DEG,

  ADC_ADC2_IN1_FWD_MV,
  ADC_ADC2_IN1_REV_MV,

  ADC_ADC3_IN3_VDIODE_MV,

} ADC_ENUM_t;


/* Event Group ADC */
typedef enum EG_ADC_ENUM {

  EG_ADC1__CONV_AVAIL_REFINT        = 0x0001U,
  EG_ADC1__CONV_AVAIL_VREF          = 0x0002U,
  EG_ADC1__CONV_AVAIL_BAT           = 0x0004U,
  EG_ADC1__CONV_AVAIL_TEMP          = 0x0008U,

  EG_ADC2__CONV_AVAIL_FWD           = 0x0010U,
  EG_ADC2__CONV_AVAIL_REV           = 0x0020U,

  EG_ADC3__CONV_AVAIL_VDIODE        = 0x0100U,

  EG_ADC1__CONV_RUNNING             = 0x1000U,
  EG_ADC2__CONV_RUNNING             = 0x2000U,
  EG_ADC3__CONV_RUNNING             = 0x4000U,

} EG_ADC_ENUM_t;


void adcMuxSelect(uint8_t mux);

void adcStartConv(ADC_ENUM_t adc);
void adcStopConv(ADC_ENUM_t adc);

#endif /* DEVICE_ADC_H_ */
