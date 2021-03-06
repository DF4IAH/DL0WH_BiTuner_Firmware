#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>

#include "prove.hpp"

#include "main.hpp"


float                                 g_adc_refint_val        = 0.0f;
float                                 g_adc_vref_mv           = 0.0f;
float                                 g_adc_bat_mv            = 0.0f;
float                                 g_adc_temp_deg          = 0.0f;
float                                 g_adc_fwd_mv            = 0.0f;
float                                 g_adc_rev_mv            = 0.0f;
float                                 g_adc_vdiode_mv         = 0.0f;
float                                 g_adc_swr               = 1e+3f;


void __disable_irq(void)
{
}

void __enable_irq(void)
{
}

void usbLog(char* buf)
{
  usbLogLen(buf, strlen(buf));
}

void usbLogLen(char* buf, int lenLog)
{
  (void) lenLog;

  printf("%s", buf);
}

void mainCalcFloat2IntFrac(float val, uint8_t fracCnt, int32_t* outInt, uint32_t* outFrac)
{
  const uint8_t isNeg = val >= 0 ?  0U : 1U;

  if (!outInt || !outFrac) {
    return;
  }

  *outInt    = (int32_t) val;
  val       -= *outInt;

  if (isNeg) {
    val = -val;
  }
  val       *= powf(10, fracCnt);
  *outFrac   = (uint32_t) (val + 0.5f);
}

float mainCalc_fwdRev_mV(float adc_mv, float vdiode_mv)
{
  return powf((M_E + (0.0f * (vdiode_mv - 500.0f))), adc_mv / 144.0f);
}

float mainCalc_VSWR(float fwd, float rev)
{
  if (fwd < 0.0f || rev < 0.0f) {
    return 1e+9f;
  }

  if (fwd <= rev) {
    return 1e+6f;
  }

  return (float) ((fwd + rev) / (fwd - rev));
}

float mainCalc_mV_to_mW(float mV)
{
  const float in_V = mV / 1000.0f;
  return 1000.0f * ((in_V * in_V) / 50.0f);
}


uint32_t osKernelSysTick(void)
{
  static uint32_t s_now = 0UL;

  return s_now += 500UL;
}



void simulator(void)
{
  /* Test impedances for AutoTune-Algo robustness check
   *          Real (Ohm)  Imag (j Ohm)    Duration Auto-Tune
   *
   *          45.00        -10.00    -->  0.390 sec (VSWR= 1: 1.084)  VERY GOOD
   *          45.00        +10.00    -->  0.210 sec (VSWR= 1: 1.063)  VERY GOOD
   *          55.00        -10.00    -->  1.320 sec (VSWR= 1: 1.092)  GOOD
   *          55.00        +10.00    -->  1.350 sec (VSWR= 1: 1.125)  FAIR

   *          30.00        -50.00    -->  1.290 sec (VSWR= 1: 1.089)  GOOD
   *          30.00        +50.00    -->  1.080 sec (VSWR= 1: 1.045)  GOOD
   *          80.00        -50.00    -->  2.640 sec (VSWR= 1: 1.033)  FAIR
   *          80.00        +50.00    -->  1.260 sec (VSWR= 1: 1.092)  GOOD

   *           5.00        -50.00    -->  2.550 sec (VSWR= 1: 1.420)  POOR
   *           5.00        +50.00    -->  0.750 sec (VSWR= 1: 1.091)  VERY GOOD
   *         500.00       -500.00    -->  2.490 sec (VSWR= 1: 1.757)  POOR
   *         500.00       +500.00    -->  2.370 sec (VSWR= 1: 2.085)  BAD
   */

  /* Groundplane with at least 3 radials horizontal
   * lambda   Real (Ohm)  Imag (j Ohm)    Duration Auto-Tune
   *
   * 0.160    12.24       -254.10    -->  2.160 sec (VSWR= 1: 1.040)  FAIR
   * 0.222    28.13        -58.57    -->  0.540 sec (VSWR= 1: 1.057)  VERY GOOD
   * 0.242    36.01         -0.22    -->  0.810 sec (VSWR= 1: 1.388)  FAIR
   * 0.250    39.71        +23.22    -->  0.420 sec (VSWR= 1: 1.080)  VERY GOOD
   * 0.270    50.68        +82.94    -->  1.080 sec (VSWR= 1: 1.049)  GOOD
   * 0.330   113.10       +299.80    -->  2.550 sec (VSWR= 1: 1.852)  POOR
   * 0.488  2380.00         +7.47    -->  2.250 sec (VSWR= 1: 3.072)  BAD
   * 0.615   112.40       -489.60    -->  2.790 sec (VSWR= 1: 1.065)  FAIR
   * 0.740    53.14         -3.30    -->  0.000 sec (VSWR= 1: 1.095)  BEST
   */

  /* Antenna */
  const float antOhmR =  30.00f;
  const float antOhmI = -50.00f;

  /* Z0 */
  const float Z0R     =  50.0f;
  const float Z0I     =  +0.0f;

  const float qrg     = 1.8e6f;  // 1.8 MHz, 160 meter band
  g_adc_vdiode_mv     = 500U;
  g_adc_refint_val    = 1638U;
  g_adc_vref_mv       = 4095U;

  /* Code from freetos.c */
  {
    /* Get the linearized voltage and (V)SWR */

    /* Simulated ADC values */
    const uint16_t adcFwd = 946U;
    uint16_t       adcRev = 850U;

    /* Get the linearized voltage */
    float l_adc_fwd_mv  = mainCalc_fwdRev_mV(adcFwd, g_adc_vdiode_mv);

    /* Get the linearized voltage and (V)SWR */
    float l_adc_rev_mv  = mainCalc_fwdRev_mV(adcRev, g_adc_vdiode_mv);
#if 0
    float l_swr         = mainCalc_VSWR(l_adc_fwd_mv, l_adc_rev_mv);
#else
    /* Simulate SWR */
    float l_swr         = controllerCalcVSWR_Simu(antOhmR, antOhmI, Z0R, Z0I, qrg);
#endif

    /* Push to global vars */
    {
      __disable_irq();
      g_adc_fwd_mv  = l_adc_fwd_mv;
      g_adc_rev_mv  = l_adc_rev_mv;
      g_adc_swr     = l_swr;
      __enable_irq();
    }
  }
}


int main(int argc, char* argv[])
{
  controllerInit();

#if 0
  printf("\r\nTEST 1:  C_val to [C] pF:\r\n");
  for (int val=0; val< 256; val++) {
    printf("C_val=%03d: C=%f pF\r\n", val, controllerCalcMatcherC2pF(val));
  }
#endif

#if 0
  printf("\r\nTEST 2:  L_val to [L] nH:\r\n");
  for (int val=0; val< 256; val++) {
    printf("C_val=%03d: L=%f nH\r\n", val, controllerCalcMatcherL2nH(val));
  }
#endif

#if 0
  printf("\r\nTEST 3:  [C] pF to C_val:\r\n");
  for (float pF = 0.0f; pF < 4400.0f; pF += 25.0f) {
    printf("C=%f pF: C_val=%03d\r\n", pF, controllerCalcMatcherPF2C(pF));
  }
#endif

#if 0
  printf("\r\nTEST 4:  [L] nH to L_val:\r\n");
  for (float nH = 0.0f; nH < 48000.0f; nH += 187.5f) {
    printf("L=%f nH: L_val=%03d\r\n", nH, controllerCalcMatcherNH2L(nH));
  }
#endif

  while (1) {
    controllerCyclicTimerEvent();

    simulator();

    //usleep(1000);
  }
}
