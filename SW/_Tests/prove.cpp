#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

//#include <vector>

#include "complex.hpp"

#include "prove.hpp"


using namespace std;


extern float                g_adc_fwd_mv;
extern float                g_adc_swr;


static const float Controller_L0_nH                           = 50.0f;
static const float Controller_Ls_nH[8]                        = {
    187.5f,
    375.0f,
    750.0f,
    1.5e+3f,
    3.0e+3f,
    6.0e+3f,
    12.0e+3f,
    24.0e+3f
};

static const float Controller_C0_pF                           = 25.0f;
static float Controller_Cp_pF[8]                              = {
    25.0f,
    //33.0f,
    50.0f,
    89.0f,
    165.0f,
    340.0f,
    600.0f,
    1137.0f,
    1939.0f
};


static const float            Controller_AutoSWR_P_mW_Min       = 5.0f;
static const float            Controller_AutoSWR_P_mW_Max       = 15.0f;
static const float            Controller_AutoSWR_SWR_Init       = 999.9f;
static const float            Controller_AutoSWR_SWR_Min        = 1.1f;
static const float            Controller_AutoSWR_SWR_Max        = 10.0f;
static const uint8_t          Controller_AutoSWR_SWR_Max_Cnt    = 30U;
static const float            Controller_AutoSWR_Time_ms_Max    = 750.0f;
static const uint8_t          Controller_AutoSWR_CVHpong_Max    = 1U;
static const uint8_t          Controller_AutoSWR_LCpong_Max     = 6U;

static uint32_t               s_controller_swr_tmr              = 0UL;
static uint32_t               s_controller_30ms_cnt             = 0UL;

static ControllerFsm_t        s_controller_FSM_state            = ControllerFsm__NOP;
static ControllerOptiCVH_t    s_controller_FSM_optiCVH          = ControllerOptiCVH__CV;
static ControllerOptiLC_t     s_controller_FSM_optiLC           = ControllerOptiLC__L;
static ControllerOptiStrat_t  s_controller_FSM_optiStrat        = ControllerOptiStrat__Double;
static ControllerOptiUpDn_t   s_controller_FSM_optiUpDn         = ControllerOptiUpDn__Up;
static uint8_t                s_controller_opti_CVHpongCtr      = 0U;
static uint8_t                s_controller_opti_LCpongCtr       = 0U;
static uint8_t                s_controller_opti_L_relays        = 0U;
static uint8_t                s_controller_opti_C_relays        = 0U;
static float                  s_controller_opti_L               = 0U;
static float                  s_controller_opti_C               = 0U;
static float                  s_controller_opti_swr_1st         = 0.0f;
static float                  s_controller_opti_swr_1st_L       = 0.0f;
static float                  s_controller_opti_swr_1st_C       = 0.0f;
static float                  s_controller_opti_swr_2nd         = 0.0f;
static float                  s_controller_opti_swr_2nd_L       = 0.0f;
static float                  s_controller_opti_swr_2nd_C       = 0.0f;
static uint8_t                s_controller_bad_swr_ctr          = 0U;
static float                  s_controller_best_swr             = 0.0f;
static float                  s_controller_best_swr_L           = 0.0f;
static float                  s_controller_best_swr_C           = 0.0f;
static ControllerOptiCVH_t    s_controller_best_swr_CVH         = ControllerOptiCVH__CV;
#if 0
static vector<float>          s_controller_xnull_L;
static vector<float>          s_controller_xnull_C;
static float                  s_controller_xnull_LC_ratio       = 0.0f;
#endif

static float                  s_controller_adc_fwd_mv           = 0.0f;
static float                  s_controller_adc_swr              = 0.0f;
static float                  s_controller_adc_fwd_mw           = 0.0f;

static bool                   s_controller_doCycle              = false;
static bool                   s_controller_doAdc                = false;


/* Calculation function */

uint32_t controllerCalcMsgHdr(ControllerMsgDestinations_t dst, ControllerMsgDestinations_t src, uint8_t lengthBytes, uint8_t cmd)
{
  return ((uint32_t)dst << 24U) | ((uint32_t)src << 16U) | ((uint32_t)lengthBytes << 8U) | ((uint32_t)cmd);
}

uint32_t controllerCalcMsgInit(uint32_t* ary, ControllerMsgDestinations_t dst, uint32_t startDelayMs)
{
  ary[0] = 0; // controllerCalcMsgHdr(dst, Destinations__Controller, sizeof(uint32_t), MsgController__InitDo);
  ary[1] = startDelayMs;
  return 2UL;
}

float controllerCalcMatcherL2nH(uint8_t Lval)
{
  float sum = Controller_L0_nH;

  for (uint8_t idx = 0; idx < 8; idx++) {
    if (Lval & (1U << idx)) {
      sum += Controller_Ls_nH[idx];
    }
  }
  return sum;
}

float controllerCalcMatcherC2pF(uint8_t Cval)
{
  float sum = Controller_C0_pF;

  for (uint8_t idx = 0; idx < 8; idx++) {
    if (Cval & (1U << idx)) {
      sum += Controller_Cp_pF[idx];
    }
  }
  return sum;
}

uint8_t controllerCalcMatcherNH2L(float nH)
{
  float   absMin  = 1e+3f;
  uint8_t valThis = 0U;

  for (uint16_t val = 0U; val < 256U; val++) {
    const float valL    = controllerCalcMatcherL2nH(val);
    const float absThis = fabs(valL - nH);

    if (absMin > absThis) {
      absMin  = absThis;
      valThis = val;
    }
  }
  return valThis;
}

uint8_t controllerCalcMatcherPF2C(float pF)
{
  float   absMin  = 1e+3f;
  uint8_t valThis = 0U;

  for (uint16_t val = 0U; val < 256U; val++) {
    const float valC    = controllerCalcMatcherC2pF(val);
    const float absThis = fabs(valC - pF);

    if (absMin > absThis) {
      absMin  = absThis;
      valThis = val;
    }
  }
  return valThis;
}

#if 0
static void controllerCalcMatcherLcRatio(void)
{
  if (!s_controller_xnull_L.empty() && !s_controller_xnull_LC_ratio) {
    const int   vCnt    = s_controller_xnull_L.size();
    const float deltaL  = s_controller_xnull_L[vCnt - 1] - s_controller_xnull_L[0];
    const float deltaC  = s_controller_xnull_C[vCnt - 1] - s_controller_xnull_C[0];

    s_controller_xnull_LC_ratio = (deltaC != 0.0f) ?  deltaL / deltaC : 0.0f;

    printf("controllerCalcMatcherLcRatio: new xnull_LC_ratio= %f, deltaL= %f, deltaC= %f.\r\n",
        s_controller_xnull_LC_ratio,
        deltaL, deltaC);
  }
}
#endif

float controllerCalcVSWR_Simu(float antOhmR, float antOhmI, float Z0R, float Z0I, float frequencyHz)
{
  const float Z0abs = sqrtf(Z0R * Z0R + Z0I * Z0I);
  const float omega = (float) (2.0 * M_PI * frequencyHz);
  float L_nH        = s_controller_opti_L;
  float C_pF        = s_controller_opti_C;
  float outR        = antOhmR;
  float outX        = antOhmI;
  float gammaRe     = 0.0f;
  float gammaIm     = 0.0f;
  float gammaAbs    = 0.0f;
  float gammaPhi    = 0.0f;
  float vswr        = Controller_AutoSWR_SWR_Init;

  /* For security only */
  if (L_nH < 1.0f) {
    L_nH = Controller_L0_nH;
  }
  if (C_pF < 1.0f) {
    C_pF = Controller_C0_pF;
  }

  if (s_controller_FSM_optiCVH == ControllerOptiCVH__CV) {
    /* Inductivity at the antenna */

    /* Serial structure */
    float LOhmX = omega * (L_nH * 1e-9f);
    outX += LOhmX;

    /* Parallel structure */
    {
      /* Invert to admitance */
      float outG = 0.0f;
      float outY = 0.0f;
      cInv(&outG, &outY, outR, outX);

      /* Capacity */
      const float COhmX = -1.0f / (omega * (C_pF * 1e-12f));
      float CSimG = 0.0f;
      float CSimY = 0.0f;
      cInv(&CSimG, &CSimY, 0.0f, COhmX);

      /* Adding C parallel to the circuit */
      outY += CSimY;

      /* Revert to impedance */
      cInv(&outR, &outX, outG, outY);
    }

  } else {
    /* Capacity at the antenna */

    /* Parallel structure */
    {
      /* Invert to admitance */
      float outG = 0.0f;
      float outY = 0.0f;
      cInv(&outG, &outY, outR, outX);

      /* Capacity */
      const float COhmX = -1.0f / (omega * (C_pF * 1e-12f));
      float CSimG = 0.0f;
      float CSimY = 0.0f;
      cInv(&CSimG, &CSimY, 0.0f, COhmX);

      /* Adding C parallel to the circuit */
      outY += CSimY;

      /* Revert to impedance */
      cInv(&outR, &outX, outG, outY);
    }

    /* Serial structure */
    float LOhmX = omega * (L_nH * 1e-9f);
    outX += LOhmX;
  }

  /* Log impedance at the transceiver */
  {
    char      buf[256];
    int32_t   trxRi, trxXi;
    uint32_t  trxRf, trxXf;

    mainCalcFloat2IntFrac(outR, 3U, &trxRi, &trxRf);
    mainCalcFloat2IntFrac(outX, 3U, &trxXi, &trxXf);

    int len = sprintf(buf,
        "VSWR_Simu: Resulting Impedance at the transceiver Z= R (%3d.%03u Ohm) + jX (%3d.%03u Ohm)\t\t having L=%5d nH and C=%5d pF, ",
        trxRi, trxRf,  trxXi, trxXf,  (uint32_t)L_nH, (uint32_t)C_pF);

    usbLogLen(buf, len);
  }

  /* Calculate Gamma = (Z - 1) / (Z + 1) */
  {
    /* Normalized Impedance */
    float outRnorm = outR / Z0abs;
    float outXnorm = outX / Z0abs;

    cDiv(&gammaRe, &gammaIm, (outRnorm - 1.0f), outXnorm, (outRnorm + 1.0f), outXnorm);

    gammaAbs = sqrtf(gammaRe * gammaRe  +  gammaIm * gammaIm);
    gammaPhi = (float) ((180.0 / M_PI) * atan2(gammaIm, gammaRe));
  }

  /* Log reflection coefficient at the transceiver */
  {
    char      buf[256];
    int32_t   gammaAbsi, gammaPhii, gammaRei, gammaImi;
    uint32_t  gammaAbsf, gammaPhif, gammaRef, gammaImf;

    mainCalcFloat2IntFrac(gammaAbs, 3U, &gammaAbsi, &gammaAbsf);
    mainCalcFloat2IntFrac(gammaPhi, 3U, &gammaPhii, &gammaPhif);
    mainCalcFloat2IntFrac(gammaRe,  3U, &gammaRei,  &gammaRef);
    mainCalcFloat2IntFrac(gammaIm,  3U, &gammaImi,  &gammaImf);

    int len = sprintf(buf,
        "|Gamma|= %1d.%03u,  Phi(Gamma)= %3d.%03u having Re(Gamma)= %3d.%03u, Im(Gamma)= %3d.%03u, ",
        gammaAbsi, gammaAbsf,  gammaPhii, gammaPhif,  gammaRei, gammaRef,  gammaImi, gammaImf);

    usbLogLen(buf, len);
  }

  /* Calculate VSWR = (1 + GammaAbs) / (1 - GammaAbs) */
  if (gammaAbs < 0.99f) {
    vswr = (1.0f + gammaAbs) / (1.0f - gammaAbs);

  } else {
    vswr = Controller_AutoSWR_SWR_Init;
  }

  /* Log reflection coefficient at the transceiver */
  {
    char buf[128];
    int32_t   vswri;
    uint32_t  vswrf;

    mainCalcFloat2IntFrac(vswr, 3U, &vswri, &vswrf);

    int len = sprintf(buf,
        "VSWR= %d.%03u.\r\n",
        vswri, vswrf);

    usbLogLen(buf, len);
  }

  return vswr;
}


/* FSM functions */

static void controllerFSM_LogAutoFinished(void)
{
  /* SWR: we have got it */

  s_controller_FSM_state = ControllerFsm__done;
  s_controller_doAdc     = true;

  /* Logging */
  {
    char buf[128];

    const int len = sprintf(buf, "Controller FSM: ControllerFsm__findImagZeroL - SWR good enough - tuner has finished.\r\n");
    usbLogLen(buf, len);
  }
}

static void controllerFSM_LogState(void)
{
  /* Show current state of optimization */
  char      buf[512];

  int32_t   s_controller_opti_swr_1st_i;
  uint32_t  s_controller_opti_swr_1st_f;

  int32_t   s_controller_opti_swr_1st_L_i;
  uint32_t  s_controller_opti_swr_1st_L_f;

  int32_t   s_controller_opti_swr_1st_C_i;
  uint32_t  s_controller_opti_swr_1st_C_f;

  int32_t   s_controller_opti_swr_2nd_i;
  uint32_t  s_controller_opti_swr_2nd_f;

  int32_t   s_controller_opti_swr_2nd_L_i;
  uint32_t  s_controller_opti_swr_2nd_L_f;

  int32_t   s_controller_opti_swr_2nd_C_i;
  uint32_t  s_controller_opti_swr_2nd_C_f;

  int       len;

  s_controller_30ms_cnt += 30UL;

  len = sprintf(buf,
                "\r\nController FSM:\tcontrollerFSM_LogState: time= %5d ms (iteration= %03u)\r\n" \
                "\ta)\t\t FSM_state= %u, optiLC= %c, optiStrat= %u, optiUpDn= %s, optiCVH= %s:\r\n" \
                "\tb)\t\t opti_L= %5u nH (%03u), opti_C= %5u pF (%03u),\r\n" \
                "\tc)\t\t opti_CVHpongCtr= %u, opti_LCpongCtr= %u, bad_swr_ctr= %02u,\r\n",
                s_controller_30ms_cnt,
                (s_controller_30ms_cnt / 30),
                s_controller_FSM_state,
                (s_controller_FSM_optiLC   == ControllerOptiLC__L ?  'L' : 'C'),
                s_controller_FSM_optiStrat,
                (s_controller_FSM_optiUpDn == ControllerOptiUpDn__Up ?  "Up" : "Dn"),
                (s_controller_FSM_optiCVH  == ControllerOptiCVH__CV  ?  "CV" : "CH"),
                (uint32_t)s_controller_opti_L, s_controller_opti_L_relays,
                (uint32_t)s_controller_opti_C, s_controller_opti_C_relays,
                s_controller_opti_CVHpongCtr, s_controller_opti_LCpongCtr, s_controller_bad_swr_ctr);
  usbLogLen(buf, len);

  mainCalcFloat2IntFrac(s_controller_opti_swr_1st,    3, &s_controller_opti_swr_1st_i,    &s_controller_opti_swr_1st_f  );
  mainCalcFloat2IntFrac(s_controller_opti_swr_1st_L,  1, &s_controller_opti_swr_1st_L_i,  &s_controller_opti_swr_1st_L_f);
  mainCalcFloat2IntFrac(s_controller_opti_swr_1st_C,  1, &s_controller_opti_swr_1st_C_i,  &s_controller_opti_swr_1st_C_f);
  mainCalcFloat2IntFrac(s_controller_opti_swr_2nd,    3, &s_controller_opti_swr_2nd_i,    &s_controller_opti_swr_2nd_f  );
  mainCalcFloat2IntFrac(s_controller_opti_swr_2nd_L,  1, &s_controller_opti_swr_2nd_L_i,  &s_controller_opti_swr_2nd_L_f);
  mainCalcFloat2IntFrac(s_controller_opti_swr_2nd_C,  1, &s_controller_opti_swr_2nd_C_i,  &s_controller_opti_swr_2nd_C_f);
  len = sprintf(buf,
                "\td)\t\t swr_1st= %2d.%03u, L= %5d.%01u nH, C= %5d.%01u pF # swr_2nd= %2d.%03u, L= %5d.%01u nH, C= %5d.%01u pF,\r\n",
                s_controller_opti_swr_1st_i,        s_controller_opti_swr_1st_f,
                s_controller_opti_swr_1st_L_i,      s_controller_opti_swr_1st_L_f,
                s_controller_opti_swr_1st_C_i,      s_controller_opti_swr_1st_C_f,
                s_controller_opti_swr_2nd_i,        s_controller_opti_swr_2nd_f,
                s_controller_opti_swr_2nd_L_i,      s_controller_opti_swr_2nd_L_f,
                s_controller_opti_swr_2nd_C_i,      s_controller_opti_swr_2nd_C_f
               );
  usbLogLen(buf, len);

  int32_t   swr_i, best_swr_i;
  uint32_t  swr_f, best_swr_f;

  mainCalcFloat2IntFrac(s_controller_adc_swr,   3, &swr_i,      &swr_f);
  mainCalcFloat2IntFrac(s_controller_best_swr,  3, &best_swr_i, &best_swr_f);
  len = sprintf(buf,
                "\te)\t\t fwd_mv=%5u mV, fwd_mw=%5u mW,\r\n" \
                "\tf)\t\t swr= %2d.%03u, best_swr= %2d.%03u @ CVH= %u: L= %5u nH, C= %5u pF.\r\n\r\n",
                (uint32_t)s_controller_adc_fwd_mv, (uint32_t)s_controller_adc_fwd_mw,
                swr_i, swr_f,  best_swr_i, best_swr_f,  s_controller_best_swr_CVH, (uint32_t)s_controller_best_swr_L, (uint32_t)s_controller_best_swr_C);
  usbLogLen(buf, len);
}

static void controllerFSM_GetGlobalVars(void)
{
  __disable_irq();
  s_controller_adc_fwd_mv     = g_adc_fwd_mv;
  s_controller_adc_swr        = g_adc_swr;
  __enable_irq();

  if (s_controller_adc_swr < Controller_AutoSWR_SWR_Init) {
    /* Add current data to the maps */
    //printf("controllerFSM_GetGlobalVars 1: adding swr= %.3f to the L/C maps.\r\n", s_controller_adc_swr);
    if (s_controller_adc_swr < s_controller_opti_swr_1st) {
      /* Move back */
      s_controller_opti_swr_2nd   = s_controller_opti_swr_1st;
      s_controller_opti_swr_2nd_L = s_controller_opti_swr_1st_L;
      s_controller_opti_swr_2nd_C = s_controller_opti_swr_1st_C;

      /* Enter */
      s_controller_opti_swr_1st   = s_controller_adc_swr;
      s_controller_opti_swr_1st_L = s_controller_opti_L;
      s_controller_opti_swr_1st_C = s_controller_opti_C;

    } else if (s_controller_adc_swr < s_controller_opti_swr_2nd) {
      /* Enter */
      s_controller_opti_swr_2nd   = s_controller_adc_swr;
      s_controller_opti_swr_2nd_L = s_controller_opti_L;
      s_controller_opti_swr_2nd_C = s_controller_opti_C;
    }
  }
  
  s_controller_adc_fwd_mw = mainCalc_mV_to_mW(s_controller_adc_fwd_mv);
#if 0
  printf("controllerFSM_GetGlobalVars: s_controller_adc_fwd_mv= %.3f mV, s_controller_adc_fwd_mw= %.3f mW, s_controller_adc_swr= %.3f\r\n",
          s_controller_adc_fwd_mv, s_controller_adc_fwd_mw, s_controller_adc_swr);
#endif
}

static void controllerFSM_PushOptiVars(void)
{
  uint8_t controller_opti_CV;
  uint8_t controller_opti_CH;
  uint32_t msgAry[2];

  /* Calculate current L and C counter setings */
  s_controller_opti_L_relays = controllerCalcMatcherNH2L(s_controller_opti_L);
  s_controller_opti_C_relays = controllerCalcMatcherPF2C(s_controller_opti_C);

  /* Update CV/CH state */
  if (s_controller_FSM_optiCVH == ControllerOptiCVH__CH) {
    controller_opti_CV  = 0U;
    controller_opti_CH  = 1U;

  } else {
    controller_opti_CV  = 1U;
    controller_opti_CH  = 0U;
  }

  /* Compose relay bitmap */
  msgAry[1] = ((uint32_t)  controller_opti_CH << 17) |
              ((uint32_t)  controller_opti_CV << 16) |
              ((uint32_t)s_controller_opti_L  <<  8) |
              ((uint32_t)s_controller_opti_C       ) ;

  /* Three extra bytes to take over */
  msgAry[0] = controllerCalcMsgHdr(Destinations__Rtos_Default, Destinations__Controller, 3, MsgDefault__SetVar03_C_L_CV_CH);
  //controllerMsgPushToOutQueue(sizeof(msgAry) / sizeof(uint32_t), msgAry, osWaitForever);
}

static void controllerFSM_StartAdc(void)
{
  s_controller_doAdc = true;
}

static bool controllerFSM_CheckPower(void)
{
  if ((Controller_AutoSWR_P_mW_Min > s_controller_adc_fwd_mw) || (s_controller_adc_fwd_mw > Controller_AutoSWR_P_mW_Max)) {
    /* Logging */
    {
      char      buf[128];
      int32_t   pwr_i;
      uint32_t  pwr_f;

      mainCalcFloat2IntFrac(s_controller_adc_fwd_mw, 3, &pwr_i, &pwr_f);
      const int len = sprintf(buf,
                              "Controller FSM: power=%5d.%03u out of [%u .. %u] Watts - stop auto tuner.\r\n",
                              pwr_i, pwr_f,
                              (uint16_t)Controller_AutoSWR_P_mW_Min, (uint16_t)Controller_AutoSWR_P_mW_Max);
      usbLogLen(buf, len);
    }

    /* Reset SWR start timer */
    s_controller_swr_tmr = osKernelSysTick();

    /* Overdrive or to low energy */
    s_controller_FSM_state = ControllerFsm__doAdc;

    return true;
  }
  return false;
}

static bool controllerFSM_CheckSwrTime(void)
{
  if (s_controller_adc_swr < Controller_AutoSWR_SWR_Min) {
    /* Logging */
    {
      char buf[128];
      int32_t   swr_i;
      uint32_t  swr_f;

      mainCalcFloat2IntFrac(s_controller_adc_swr, 3, &swr_i, &swr_f);
      const int len = sprintf(buf,
                              "Controller FSM: VSWR= %2d.%03u is good enough - stop auto tuner.\r\n",
                              swr_i, swr_f);
      usbLogLen(buf, len);
    }

    /* No need to start */
    s_controller_FSM_state = ControllerFsm__doAdc;

    return true;

  } else if (Controller_AutoSWR_Time_ms_Max > (osKernelSysTick() - s_controller_swr_tmr)) {
    /* Timer has not yet elapsed */
    s_controller_FSM_state = ControllerFsm__doAdc;

    return true;
  }
  return false;
}

static void controllerFSM_SwitchOverCVH(void)
{
  /* Check if another CVH switch over is allowed */
  if (++s_controller_opti_CVHpongCtr <= Controller_AutoSWR_CVHpong_Max) {
    s_controller_opti_LCpongCtr = 0U;
    s_controller_bad_swr_ctr    = 0U;

    /* Logging */
    {
      char      buf[128];
      const int len = sprintf(buf,
                              "Controller FSM: switch over the constellation to %d (0: CV, 1: CH), CVH pingpong ctr=%u.\r\n",
                              !s_controller_FSM_optiCVH, s_controller_opti_CVHpongCtr);
      usbLogLen(buf, len);
    }

    /* Switch over to opposite CVH constellation */
    s_controller_FSM_optiCVH      = (s_controller_FSM_optiCVH == ControllerOptiCVH__CV) ?  ControllerOptiCVH__CH : ControllerOptiCVH__CV;
    if (s_controller_FSM_optiCVH == ControllerOptiCVH__CH) {
      s_controller_FSM_optiLC     = ControllerOptiLC__C;
      s_controller_opti_C         = Controller_C0_pF + Controller_Cp_pF[0];

    } else {
      s_controller_FSM_optiLC     = ControllerOptiLC__L;
      s_controller_opti_L         = Controller_L0_nH + Controller_Ls_nH[0];
    }

    s_controller_FSM_optiStrat    = ControllerOptiStrat__Double;
    s_controller_FSM_optiUpDn     = ControllerOptiUpDn__Up;
    s_controller_FSM_state        = ControllerFsm__findImagZero;

    /* Erase array for new L/C combinations */
    s_controller_opti_swr_1st     = Controller_AutoSWR_SWR_Init;
    s_controller_opti_swr_2nd     = Controller_AutoSWR_SWR_Init;

  } else {
    /* Exhausted - start over again */
    s_controller_FSM_state        = ControllerFsm__done;
    s_controller_doAdc            = true;
  }
}

static void controllerFSM_DoubleStrategy(void)
{
  if (s_controller_FSM_optiLC == ControllerOptiLC__L) {
    //const float delta_L = s_controller_opti_L;

    /* Double L */
    s_controller_opti_L += s_controller_opti_L;

    #if 0
    /* Parallel strategy */
    if (s_controller_xnull_LC_ratio != 0.0f) {
      s_controller_opti_C += 0.1f * (delta_L / s_controller_xnull_LC_ratio);
    }
    #endif

  } else {
    //const float delta_C = s_controller_opti_C;

    /* Double C */
    s_controller_opti_C += s_controller_opti_C;

    #if 0
    /* Parallel strategy */
    if (s_controller_xnull_LC_ratio != 0.0f) {
      s_controller_opti_C += 0.1f * (delta_C * s_controller_xnull_LC_ratio);
    }
    #endif
  }
}

static void controllerFSM_HalfStrategy(void)
{
  if (s_controller_FSM_optiLC == ControllerOptiLC__L) {
    //const float lastL = s_controller_opti_L;

    /* Mean L */
    s_controller_opti_L = (s_controller_opti_swr_1st_L + s_controller_opti_swr_2nd_L) / 2.0f;

    #if 0
    /* Parallel strategy */
    if (s_controller_xnull_LC_ratio != 0.0f) {
      const float deltaL = s_controller_opti_L - lastL;

      s_controller_opti_C *= deltaL / s_controller_xnull_LC_ratio;
    }
    #endif

  } else {
    //const float lastC = s_controller_opti_C;

    /* Mean C */
    s_controller_opti_C = (s_controller_opti_swr_1st_C + s_controller_opti_swr_2nd_C) / 2.0f;

    #if 0
    /* Parallel strategy */
    if (s_controller_xnull_LC_ratio != 0.0f) {
      const float deltaC = s_controller_opti_C - lastC;

      s_controller_opti_C *= deltaC * s_controller_xnull_LC_ratio;
    }
    #endif
  }
}

static void controllerFSM_ZeroXHalfStrategy(void)
{
  if (s_controller_FSM_optiStrat != ControllerOptiStrat__Half) {
    return;
  }

  /* Work out the half strategy */
  controllerFSM_HalfStrategy();

  if (s_controller_FSM_optiLC == ControllerOptiLC__L) {
    const float L_diff_abs = fabs(s_controller_opti_swr_1st_L - s_controller_opti_swr_2nd_L);
    
    printf("controllerFSM_ZeroXHalfStrategy 1: abs(swr_1st_L - swr_2nd_L)=%f\r\n", L_diff_abs);
    if (L_diff_abs <= Controller_Ls_nH[0]) {
      #if 0
      /* First zero X found */
      s_controller_xnull_L.push_back(s_controller_opti_L);
      s_controller_xnull_C.push_back(s_controller_opti_C);
      #endif

      /* Check if optimum found by increase of L */
      if (s_controller_opti_swr_1st_L > Controller_Ls_nH[0]) {
        /* Advance L, at least by one step, limiting */
        const float advanced = s_controller_opti_swr_1st_L * 1.4f + Controller_Ls_nH[0];
        if (advanced > 2.0f * Controller_Ls_nH[7]) {
          s_controller_opti_L = controllerCalcMatcherNH2L( controllerCalcMatcherL2nH(advanced) );

        } else {
          s_controller_opti_L = advanced;
        }

        /* Next optimize C */
        s_controller_opti_C         = Controller_C0_pF + Controller_Cp_pF[0];
        s_controller_opti_LCpongCtr = 0U;
        s_controller_FSM_optiLC     = ControllerOptiLC__C;
        s_controller_FSM_optiStrat  = ControllerOptiStrat__Double;
        s_controller_FSM_optiUpDn   = ControllerOptiUpDn__Up;
        s_controller_FSM_state      = ControllerFsm__findMinSwr;

        /* Forget this minimum due to C advance */
        s_controller_adc_swr        = Controller_AutoSWR_SWR_Init;

        /* Erase array for new L/C combinations */
        s_controller_opti_swr_1st   = Controller_AutoSWR_SWR_Init;
        s_controller_opti_swr_2nd   = Controller_AutoSWR_SWR_Init;
        
      } else {
        /* Switch over to opposite CVH constellation */
        controllerFSM_SwitchOverCVH();
      }

    }
    
  } else {
    const float C_diff_abs = fabs(s_controller_opti_swr_1st_C - s_controller_opti_swr_2nd_C);
    
    printf("controllerFSM_ZeroXHalfStrategy 2: abs(swr_1st_C - swr_2nd_C)=%f\r\n", C_diff_abs);
    if (C_diff_abs <= Controller_Cp_pF[0]) {
      #if 0
      /* First zero X found */
      s_controller_xnull_C.push_back(s_controller_opti_C);
      s_controller_xnull_L.push_back(s_controller_opti_L);
      #endif

      /* Check if optimum found by increase of C */
      if (s_controller_opti_swr_1st_C > Controller_Cp_pF[0]) {
        /* Advance C, at least by one step, limiting */
        const float advanced = s_controller_opti_swr_1st_C * 1.4f + Controller_Cp_pF[0];
        if (advanced > 2.0f * Controller_Cp_pF[7]) {
          s_controller_opti_C = controllerCalcMatcherPF2C( controllerCalcMatcherC2pF(advanced) );
        } else {
          s_controller_opti_C = advanced;
        }

        /* Next optimize L */
        s_controller_opti_L         = Controller_L0_nH + Controller_Ls_nH[0];
        s_controller_opti_LCpongCtr = 0U;
        s_controller_FSM_optiLC     = ControllerOptiLC__L;
        s_controller_FSM_optiStrat  = ControllerOptiStrat__Double;
        s_controller_FSM_optiUpDn   = ControllerOptiUpDn__Up;
        s_controller_FSM_state      = ControllerFsm__findMinSwr;

        /* Forget this minimum due to L advance */
        s_controller_adc_swr        = Controller_AutoSWR_SWR_Init;

        /* Erase array for new L/C combinations */
        s_controller_opti_swr_1st   = Controller_AutoSWR_SWR_Init;
        s_controller_opti_swr_2nd   = Controller_AutoSWR_SWR_Init;
        
      } else {
        /* Switch over to opposite CVH constellation */
        controllerFSM_SwitchOverCVH();
      }
    }
  }
}

static void controllerFSM_OptiHalfStrategy(void)
{
  if (s_controller_FSM_optiStrat != ControllerOptiStrat__Half) {
    return;
  }
  
  /* Work out the half strategy */
  controllerFSM_HalfStrategy();

  if (s_controller_FSM_optiLC == ControllerOptiLC__L) {
    const float L_diff_abs = fabs(s_controller_opti_swr_1st_L - s_controller_opti_swr_2nd_L);

    printf("controllerFSM_OptiHalfStrategy 1: abs(swr_1st_L - swr_2nd_L)=%f\r\n", L_diff_abs);
    if (L_diff_abs <= Controller_Ls_nH[0]) {
      #if 0
      /* Again, zero X found */
      s_controller_xnull_L.push_back(s_controller_opti_L);
      s_controller_xnull_C.push_back(s_controller_opti_C);
      controllerCalcMatcherLcRatio();
      #endif

      if (++s_controller_opti_LCpongCtr < Controller_AutoSWR_LCpong_Max) {
        /* Optimize C, again */
        s_controller_opti_C         = Controller_C0_pF + Controller_Cp_pF[0];
        s_controller_FSM_optiLC     = ControllerOptiLC__C;
        s_controller_FSM_optiStrat  = ControllerOptiStrat__Double;
        s_controller_FSM_optiUpDn   = ControllerOptiUpDn__Up;
        s_controller_FSM_state      = ControllerFsm__findMinSwr;

        /* Erase array for new L/C combinations */
        s_controller_opti_swr_1st   = Controller_AutoSWR_SWR_Init;
        s_controller_opti_swr_2nd   = Controller_AutoSWR_SWR_Init;

      } else {
        /* Switch over to opposite CVH constellation */
        controllerFSM_SwitchOverCVH();
      }
    }

  } else {
    const float C_diff_abs = fabs(s_controller_opti_swr_1st_C - s_controller_opti_swr_2nd_C);

    printf("controllerFSM_OptiHalfStrategy 2: abs(swr_1st_C - swr_2nd_C)=%f\r\n", C_diff_abs);
    if (C_diff_abs <= Controller_Cp_pF[0]) {
      #if 0
      /* Again, zero X found */
      s_controller_xnull_L.push_back(s_controller_opti_L);
      s_controller_xnull_C.push_back(s_controller_opti_C);
      controllerCalcMatcherLcRatio();
      #endif

      if (++s_controller_opti_LCpongCtr < Controller_AutoSWR_LCpong_Max) {
        /* Optimize L, again */
        s_controller_opti_L         = Controller_L0_nH + Controller_Ls_nH[0];
        s_controller_FSM_optiLC     = ControllerOptiLC__L;
        s_controller_FSM_optiStrat  = ControllerOptiStrat__Double;
        s_controller_FSM_optiUpDn   = ControllerOptiUpDn__Up;
        s_controller_FSM_state      = ControllerFsm__findMinSwr;

        /* Erase array for new L/C combinations */
        s_controller_opti_swr_1st   = Controller_AutoSWR_SWR_Init;
        s_controller_opti_swr_2nd   = Controller_AutoSWR_SWR_Init;

      } else {
        /* Switch over to opposite CVH constellation */
        controllerFSM_SwitchOverCVH();
      }
    }
  }
}

static void controllerFSM(void)
{
  switch (s_controller_FSM_state)
  {

  case ControllerFsm__NOP:
  case ControllerFsm__doAdc:
  {
    /* Init SWR compare value */
    s_controller_adc_swr = Controller_AutoSWR_SWR_Init;

    /* Erase array for new L/C combinations */
    s_controller_opti_swr_1st   = Controller_AutoSWR_SWR_Init;
    s_controller_opti_swr_2nd   = Controller_AutoSWR_SWR_Init;

    #if 0
    /* Erase Xnull vector */
    s_controller_xnull_L.clear();
    s_controller_xnull_C.clear();
    s_controller_xnull_LC_ratio = 0.0f;
    #endif

    controllerFSM_StartAdc();

    s_controller_FSM_state = ControllerFsm__startAuto;
  }
    break;

  case ControllerFsm__startAuto:
  {
    /* Pull global vars */
    controllerFSM_GetGlobalVars();

    /* Check for security */
    if (controllerFSM_CheckPower()) {
      break;
    }

    /* Check if auto tuner should start */
    if (controllerFSM_CheckSwrTime()) {
      break;
    }

    /* Run (V)SWR optimization */
    s_controller_FSM_optiCVH      = ControllerOptiCVH__CV;
    s_controller_FSM_optiLC       = ControllerOptiLC__L;
    s_controller_FSM_optiStrat    = ControllerOptiStrat__Double;
    s_controller_FSM_optiUpDn     = ControllerOptiUpDn__Up;
    s_controller_FSM_state        = ControllerFsm__findImagZero;
    s_controller_opti_CVHpongCtr  = s_controller_opti_LCpongCtr = s_controller_bad_swr_ctr = 0U;
    s_controller_best_swr         = s_controller_adc_swr = Controller_AutoSWR_SWR_Init;
    s_controller_opti_L           = Controller_L0_nH + Controller_Ls_nH[0];
    s_controller_opti_C           = Controller_C0_pF;

    /* Logging */
    {
      char buf[128];

      const int len = sprintf(buf, "Controller FSM: ControllerFsm__startAuto - start auto tuner.\r\n");
      usbLogLen(buf, len);
    }

    /* Push opti data to relays */
    controllerFSM_PushOptiVars();
    controllerFSM_StartAdc();
  }
    break;

  case ControllerFsm__findImagZero:
  {
    /* Pull global vars */
    controllerFSM_GetGlobalVars();

    /* Check for security */
    if (controllerFSM_CheckPower()) {
      break;
    }

    /* Show current state of optimization */
    controllerFSM_LogState();

    /* Check if SWR is not usable in that constellation */
    if (s_controller_adc_swr > Controller_AutoSWR_SWR_Max) {
      if (s_controller_bad_swr_ctr++ >= Controller_AutoSWR_SWR_Max_Cnt) {
        /* Switch over to opposite CVH constellation and restart of growing L and C */
        s_controller_opti_L           = Controller_L0_nH + Controller_Ls_nH[0];
        s_controller_opti_C           = Controller_C0_pF + Controller_Cp_pF[0];
        controllerFSM_SwitchOverCVH();
        break;
      }

    } else {
      if (s_controller_bad_swr_ctr > 0UL) {
        s_controller_bad_swr_ctr--;
      }
    }

    /* Check for SWR */
    if (s_controller_adc_swr == s_controller_opti_swr_1st) {
      /* SWR got better */

      if (s_controller_adc_swr <= Controller_AutoSWR_SWR_Min) {
        /* SWR: we have got it */
        controllerFSM_LogAutoFinished();
        break;
      }

    } else {
      /* SWR got worse */

      if (s_controller_FSM_optiStrat == ControllerOptiStrat__Double) {
        /* Change strategy */
        s_controller_FSM_optiStrat = ControllerOptiStrat__Half;
        s_controller_FSM_optiUpDn  = ControllerOptiUpDn__Dn;
      }
    }

    /* Execute the strategy */
    if (s_controller_FSM_optiStrat == ControllerOptiStrat__Double) {
      /* Double L/C for quick access */
      controllerFSM_DoubleStrategy();

    } else if (s_controller_FSM_optiStrat == ControllerOptiStrat__Half) {
      /* Half strategy for zero X configuration */
      controllerFSM_ZeroXHalfStrategy();
    }

    /* Push opti data to relays */
    controllerFSM_PushOptiVars();
    controllerFSM_StartAdc();
  }
    break;

  case ControllerFsm__findMinSwr:
  {
    /* Pull global vars */
    controllerFSM_GetGlobalVars();

    /* Check for security */
    if (controllerFSM_CheckPower()) {
      break;
    }

    /* Show current state of optimization */
    controllerFSM_LogState();

    /* Check if SWR is not usable in that constellation */
    if (s_controller_adc_swr > Controller_AutoSWR_SWR_Max) {
      if (s_controller_bad_swr_ctr++ >= Controller_AutoSWR_SWR_Max_Cnt) {
        /* Switch over to opposite CVH constellation and restart of growing L and C */
        s_controller_opti_L           = Controller_L0_nH + Controller_Ls_nH[0];
        s_controller_opti_C           = Controller_C0_pF + Controller_Cp_pF[0];
        controllerFSM_SwitchOverCVH();
        break;
      }

    } else {
      /* Good SWR drops one bad */
      if (s_controller_bad_swr_ctr > 0UL) {
        s_controller_bad_swr_ctr--;
      }
    }

    /* Check for SWR */
    if (s_controller_adc_swr == s_controller_opti_swr_1st) {
      /* SWR got better */

      if (s_controller_adc_swr <= Controller_AutoSWR_SWR_Min) {
        /* SWR: we have got it */
        controllerFSM_LogAutoFinished();
        break;
      }

    } else {
      /* SWR got worse */

      if (s_controller_FSM_optiStrat == ControllerOptiStrat__Double) {
        /* Change strategy */
        s_controller_FSM_optiStrat = ControllerOptiStrat__Half;
        s_controller_FSM_optiUpDn  = ControllerOptiUpDn__Dn;
      }
    }

    /* Execute the strategy */
    if (s_controller_FSM_optiStrat == ControllerOptiStrat__Double) {
      /* Double L/C for quick access */
      controllerFSM_DoubleStrategy();

    } else if (s_controller_FSM_optiStrat == ControllerOptiStrat__Half) {
      /* Half strategy */
      controllerFSM_OptiHalfStrategy();
    }
        
    /* Push opti data to relays */
    controllerFSM_PushOptiVars();
    controllerFSM_StartAdc();
  }
    break;

  case ControllerFsm__done:
  {
    /* Take the best result */
    s_controller_FSM_optiCVH  = s_controller_best_swr_CVH;
    s_controller_opti_L       = s_controller_best_swr_L;
    s_controller_opti_C       = s_controller_best_swr_C;

    /* Show current state of optimization */
    controllerFSM_LogState();

    /* Push opti data to relays */
    controllerFSM_PushOptiVars();

    /* Check if current VSWR is good enough */
    if (s_controller_adc_swr > Controller_AutoSWR_SWR_Min) {
      /* Done - wait for new start */
      s_controller_FSM_state = ControllerFsm__doAdc;
    }

    controllerFSM_StartAdc();
    exit(0);  // TODO: remove me!
  }
    break;

  default:
  {
    s_controller_FSM_state = ControllerFsm__NOP;
  }

  }  // switch ()
}

/* Cyclic job to do */
void controllerCyclicTimerEvent(void)
{
  uint32_t  msgAry[16];
  uint8_t   msgLen = 0U;

  /* FSM logic */
  controllerFSM();
#if 0
  printf("TimerEvent: s_controller_FSM_state= %d\r\n", s_controller_FSM_state);
#endif

  if (s_controller_doAdc) {
    /* Get ADC channels */
    msgAry[msgLen++]  = controllerCalcMsgHdr(Destinations__Rtos_Default, Destinations__Controller, 0U, MsgDefault__CallFunc01_MCU_ADC1);
    msgAry[msgLen++]  = controllerCalcMsgHdr(Destinations__Rtos_Default, Destinations__Controller, 0U, MsgDefault__CallFunc02_MCU_ADC3_VDIODE);
    msgAry[msgLen++]  = controllerCalcMsgHdr(Destinations__Rtos_Default, Destinations__Controller, 0U, MsgDefault__CallFunc03_MCU_ADC2_FWD);
    msgAry[msgLen++]  = controllerCalcMsgHdr(Destinations__Rtos_Default, Destinations__Controller, 0U, MsgDefault__CallFunc04_MCU_ADC2_REV);
  }

  /* Push to queue */
  //controllerMsgPushToInQueue(msgLen, msgAry, 1UL);
}

void controllerInit(void)
{
  /* Set relay state */
  controllerFSM_PushOptiVars();

  /* Reset SWR start timer */
  s_controller_swr_tmr = osKernelSysTick();
}
