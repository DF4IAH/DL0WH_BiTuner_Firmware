/*
 * task_UART.c
 *
 *  Created on: 28.01.2019
 *      Author: DF4IAH
 */

/* Includes ------------------------------------------------------------------*/
#include <stddef.h>
#include <sys/_stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32l4xx_hal.h"
#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "task.h"
#include "queue.h"
#include "usart.h"

#include "task_Controller.h"
#include "task_CAT.h"

#include "task_UART.h"


/* Variables -----------------------------------------------------------------*/
extern osMessageQId         uartTxQueueHandle;
extern osMessageQId         uartRxQueueHandle;

extern osSemaphoreId        uart_BSemHandle;
extern osSemaphoreId        c2uartTx_BSemHandle;
extern osSemaphoreId        c2uartRx_BSemHandle;

extern EventGroupHandle_t   globalEventGroupHandle;
extern EventGroupHandle_t   uartEventGroupHandle;


volatile uint8_t            g_uartTxDmaBuf[256]                     = { 0U };

volatile uint8_t            g_uartRxDmaBuf[256]                     = { 0U };
uint32_t                    g_uartRxDmaBufLast                      = 0UL;
uint32_t                    g_uartRxDmaBufIdx                       = 0UL;

static uint8_t              s_uartTx_enable                         = 0U;
static uint32_t             s_uartTxStartTime                       = 0UL;
static osThreadId           s_uartTxPutterTaskHandle                = 0;

static uint8_t              s_uartRx_enable                         = 0U;
static uint32_t             s_uartRxStartTime                       = 0UL;
static osThreadId           s_uartRxGetterTaskHandle                = 0;



/* HAL callbacks for the UART */

/**
  * @brief  Tx Transfer completed callback
  * @param  UartHandle: UART handle.
  * @note   This example shows a simple way to report end of DMA Tx transfer, and
  *         you can add your own implementation.
  * @retval None
  */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *UartHandle)
{
  if (UartHandle == &huart4) {
    HAL_CAT_TxCpltCallback(UartHandle);
    return;
  }

  /* Set flags: DMA complete */
  xEventGroupSetBits(  uartEventGroupHandle, UART_EG__DMA_TX_END);
  xEventGroupClearBits(uartEventGroupHandle, UART_EG__DMA_TX_RUN);
}

/**
  * @brief  Rx Transfer completed callback
  * @param  UartHandle: UART handle
  * @note   This example shows a simple way to report end of DMA Rx transfer, and
  *         you can add your own implementation.
  * @retval None
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *UartHandle)
{
  if (UartHandle == &huart4) {
    HAL_CAT_RxCpltCallback(UartHandle);
    return;
  }

  /* Set flags: DMA complete */
  xEventGroupClearBits(uartEventGroupHandle, UART_EG__DMA_RX_RUN);
  xEventGroupSetBits(  uartEventGroupHandle, UART_EG__DMA_RX_END);
}

/**
  * @brief  UART error callbacks
  * @param  UartHandle: UART handle
  * @note   This example shows a simple way to report transfer error, and you can
  *         add your own implementation.
  * @retval None
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *UartHandle)
{
  if (UartHandle == &huart4) {
    HAL_CAT_ErrorCallback(UartHandle);
    return;
  }

  Error_Handler();
}


/* UART HAL Init */

static void uartHalInit(void)
{
  _Bool already = false;

  /* Block when already called */
  {
    taskENTER_CRITICAL();
    if (s_uartTx_enable || s_uartRx_enable) {
      already = true;
    }
    taskEXIT_CRITICAL();

    if (already) {
      return;
    }
  }

  /* Init UART */
  {
    hlpuart1.Init.BaudRate                = 9600;
    hlpuart1.Init.WordLength              = UART_WORDLENGTH_8B;
    hlpuart1.Init.StopBits                = UART_STOPBITS_1;
    hlpuart1.Init.Parity                  = UART_PARITY_NONE;
    hlpuart1.Init.HwFlowCtl               = UART_HWCONTROL_NONE;
    hlpuart1.Init.Mode                    = UART_MODE_TX_RX;
    hlpuart1.AdvancedInit.AdvFeatureInit  = UART_ADVFEATURE_NO_INIT;

    if(HAL_UART_DeInit(&hlpuart1) != HAL_OK)
    {
      Error_Handler();
    }
    if(HAL_UART_Init(&hlpuart1) != HAL_OK)
    {
      Error_Handler();
    }
  }

}


/* UART TX */

const uint8_t uartTx_MaxWaitQueueMs = 100U;
static void uartTxPush(const uint8_t* buf, uint32_t len)
{
  if (buf && len) {
    while (len--) {
      osMessagePut(uartTxQueueHandle, *(buf++), uartTx_MaxWaitQueueMs);
    }
    osMessagePut(uartTxQueueHandle, 0, uartTx_MaxWaitQueueMs);
  }
}

const uint16_t uartTxWait_MaxWaitSemMs = 500U;
static void uartTxPushWait(const uint8_t* buf, uint32_t len)
{
  EventBits_t eb = xEventGroupWaitBits(uartEventGroupHandle, UART_EG__TX_BUF_EMPTY, 0UL, 0, uartTxWait_MaxWaitSemMs);
  if (eb & UART_EG__TX_BUF_EMPTY) {
    uartTxPush(buf, len);
  }
}

void uartLogLen(const char* str, int len)
{
  if (s_uartTx_enable) {
    osSemaphoreWait(uart_BSemHandle, 0UL);
    uartTxPushWait((uint8_t*)str, len);
    osSemaphoreRelease(uart_BSemHandle);
  }
}

inline
void uartLog(const char* str)
{
  uartLogLen(str, strlen(str));
}


static void uartTxStartDma(const uint8_t* cmdBuf, uint8_t cmdLen)
{
  /* When TX is running wait for the end of the previous transfer */
  if (UART_EG__DMA_TX_RUN & xEventGroupGetBits(uartEventGroupHandle)) {
    /* Block until end of transmission - at max. 1 sec */
    xEventGroupWaitBits(uartEventGroupHandle,
        UART_EG__DMA_TX_END,
        UART_EG__DMA_TX_END,
        0, 1000 / portTICK_PERIOD_MS);
  }

  /* Copy to DMA TX buffer */
  memcpy((uint8_t*)g_uartTxDmaBuf, cmdBuf, cmdLen);
  memset((uint8_t*)g_uartTxDmaBuf + cmdLen, 0, sizeof(g_uartTxDmaBuf) - cmdLen);

  /* Re-set flag for TX */
  xEventGroupSetBits(uartEventGroupHandle, UART_EG__DMA_TX_RUN);

  /* Start transmission */
  if (HAL_OK != HAL_UART_Transmit_DMA(&hlpuart1, (uint8_t*)g_uartTxDmaBuf, cmdLen))
  {
    Error_Handler();
  }
}

void uartTxPutterTask(void const * argument)
{
  const uint8_t   maxWaitMs   = 25U;
  uint8_t         buf[256];

  const uint32_t  lastBuf     = sizeof(buf) - 1;

  /* Clear queue */
  while (osMessageGet(uartTxQueueHandle, 1UL).status == osEventMessage) {
  }
  xEventGroupSetBits(uartEventGroupHandle, UART_EG__TX_BUF_EMPTY);

  /* TaskLoop */
  for (;;) {
    uint8_t len = 0U;

    uint8_t* bufPtr = buf;
    for (uint32_t idx = 0UL; idx < lastBuf; idx++, bufPtr++) {
      osEvent ev = osMessageGet(uartTxQueueHandle, maxWaitMs);
      if (ev.status == osEventMessage) {
        /* Group-Bit for empty queue cleared */
        xEventGroupClearBits(uartEventGroupHandle, UART_EG__TX_BUF_EMPTY);

        *bufPtr = (uint8_t) ev.value.v;

      } else {
        *(++bufPtr) = 0U;
        len = idx;

        break;
      }
    }
    buf[lastBuf] = 0U;

    /* Start DMA TX engine */
    if (len) {
      uartTxStartDma(buf, len);

      /* Group-Bit for empty queue set */
      xEventGroupSetBits(uartEventGroupHandle, UART_EG__TX_BUF_EMPTY);

    } else {
      /* Delay for the next attempt */
      osDelay(25UL);
    }
  }
}

static void uartTxInit(void)
{
  /* Init UART HAL */
  uartHalInit();

  /* Start putter thread */
  osThreadDef(uartTxPutterTask, uartTxPutterTask, osPriorityHigh, 0, 128);
  s_uartTxPutterTaskHandle = osThreadCreate(osThread(uartTxPutterTask), NULL);
}

static void uartTxMsgProcess(uint32_t msgLen, const uint32_t* msgAry)
{
  uint32_t                msgIdx  = 0UL;
  const uint32_t          hdr     = msgAry[msgIdx++];
  const UartCmds_t        cmd     = (UartCmds_t) (0xffUL & hdr);

  switch (cmd) {
  case MsgUartTx__InitDo:
    {
      /* Start at defined point of time */
      const uint32_t delayMs = msgAry[msgIdx++];
      if (delayMs) {
        uint32_t  previousWakeTime = s_uartTxStartTime;
        osDelayUntil(&previousWakeTime, delayMs);
      }

      /* Init module */
      uartTxInit();

      /* Activation flag */
      s_uartTx_enable = 1U;

      /* Return Init confirmation */
      uint32_t cmdBack[1];
      cmdBack[0] = controllerCalcMsgHdr(Destinations__Controller, Destinations__Network_UartTx, 0U, MsgUartTx__InitDone);
      controllerMsgPushToInQueue(sizeof(cmdBack) / sizeof(int32_t), cmdBack, osWaitForever);
    }
    break;

  default: { }
  }  // switch (cmd)
}

void uartTxTaskInit(void)
{
  /* Wait until controller is up */
  xEventGroupWaitBits(globalEventGroupHandle,
      EG_GLOBAL__Controller_CTRL_IS_RUNNING,
      0UL,
      0, portMAX_DELAY);

  /* Store start time */
  s_uartTxStartTime = osKernelSysTick();

  /* Give other tasks time to do the same */
  osDelay(10UL);
}

void uartTxTaskLoop(void)
{
  uint32_t  msgLen                        = 0UL;
  uint32_t  msgAry[CONTROLLER_MSG_Q_LEN];

  /* Wait for door bell and hand-over controller out queue */
  {
    osSemaphoreWait(c2uartTx_BSemHandle, osWaitForever);
    msgLen = controllerMsgPullFromOutQueue(msgAry, Destinations__Network_UartTx, 1UL);                  // Special case of callbacks need to limit blocking time
  }

  /* Decode and execute the commands when a message exists
   * (in case of callbacks the loop catches its wakeup semaphore
   * before ctrlQout is released results to request on an empty queue) */
  if (msgLen) {
    uartTxMsgProcess(msgLen, msgAry);
  }
}


/* UART RX */

static void uartRxStartDma(void)
{
  const uint16_t dmaBufSize = sizeof(g_uartRxDmaBuf);

  /* Reset working indexes */
  g_uartRxDmaBufLast = g_uartRxDmaBufIdx = 0U;

  /* Start RX DMA */
  if (HAL_UART_Receive_DMA(&hlpuart1, (uint8_t*)g_uartRxDmaBuf, dmaBufSize) != HAL_OK)
  {
    //Error_Handler();
  }

  /* Set RX running flag */
  xEventGroupSetBits(uartEventGroupHandle, UART_EG__DMA_RX_RUN);
}


void uartRxGetterTask(void const * argument)
{
  const uint8_t nulBuf[1]   = { 0U };
  const uint8_t maxWaitMs   = 25U;

  /* TaskLoop */
  for (;;) {
    taskDISABLE_INTERRUPTS();

    /* Find last written byte */
    g_uartRxDmaBufIdx = g_uartRxDmaBufLast + strnlen((char*)g_uartRxDmaBuf + g_uartRxDmaBufLast, sizeof(g_uartRxDmaBuf) - g_uartRxDmaBufLast);

    /* Send new character in RX buffer to the queue */
    if (g_uartRxDmaBufIdx > g_uartRxDmaBufLast) {
      /* USB OUT EP from host put data into the buffer */
      volatile uint8_t* bufPtr = g_uartRxDmaBuf + g_uartRxDmaBufLast;

      for (int32_t idx = g_uartRxDmaBufLast + 1U; idx <= g_uartRxDmaBufIdx; ++idx, ++bufPtr) {
        xQueueSendToBack(uartRxQueueHandle, (uint8_t*)bufPtr, maxWaitMs);
      }
      xQueueSendToBack(uartRxQueueHandle, nulBuf, maxWaitMs);

      taskENABLE_INTERRUPTS();

    } else {
      /* Delay for the next attempt */
      osDelay(25UL);
    }

    /* Restart DMA if transfer has finished */
    if (UART_EG__DMA_RX_END & xEventGroupGetBits(uartEventGroupHandle)) {
      /* Clear DMA buffer */
      memset((char*) g_uartRxDmaBuf, 0, sizeof(g_uartRxDmaBuf));
      g_uartRxDmaBufLast = g_uartRxDmaBufIdx = 0UL;

      xEventGroupClearBits(uartEventGroupHandle, UART_EG__DMA_RX_END);

      /* Reactivate UART RX DMA transfer */
      uartRxStartDma();
    }
  }
}

static void uartRxInit(void)
{
  /* Init UART HAL */
  uartHalInit();

  /* Start getter thread */
  osThreadDef(uartRxGetterTask, uartRxGetterTask, osPriorityHigh, 0, 128);
  s_uartRxGetterTaskHandle = osThreadCreate(osThread(uartRxGetterTask), NULL);

  /* Activate UART RX DMA transfer */
  if (HAL_UART_Receive_DMA(&hlpuart1, (uint8_t*) g_uartRxDmaBuf, sizeof(g_uartRxDmaBuf) - 1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void uartRxMsgProcess(uint32_t msgLen, const uint32_t* msgAry)
{
  uint32_t                msgIdx  = 0UL;
  const uint32_t          hdr     = msgAry[msgIdx++];
  const UartCmds_t        cmd     = (UartCmds_t) (0xffUL & hdr);

  switch (cmd) {
  case MsgUartRx__InitDo:
    {
      /* Start at defined point of time */
      const uint32_t delayMs = msgAry[msgIdx++];
      if (delayMs) {
        uint32_t  previousWakeTime = s_uartRxStartTime;
        osDelayUntil(&previousWakeTime, delayMs);
      }

      /* Init module */
      uartRxInit();

      /* Activation flag */
      s_uartRx_enable = 1U;

      /* Return Init confirmation */
      uint32_t cmdBack[1];
      cmdBack[0] = controllerCalcMsgHdr(Destinations__Controller, Destinations__Network_UartRx, 0U, MsgUartRx__InitDone);
      controllerMsgPushToInQueue(sizeof(cmdBack) / sizeof(int32_t), cmdBack, osWaitForever);
    }
    break;

  default: { }
  }  // switch (cmd)
}

void uartRxTaskInit(void)
{
  /* Wait until controller is up */
  xEventGroupWaitBits(globalEventGroupHandle,
      EG_GLOBAL__Controller_CTRL_IS_RUNNING,
      0UL,
      0, portMAX_DELAY);

  /* Store start time */
  s_uartRxStartTime = osKernelSysTick();

  /* Give other tasks time to do the same */
  osDelay(10UL);
}

void uartRxTaskLoop(void)
{
  uint32_t  msgLen                        = 0UL;
  uint32_t  msgAry[CONTROLLER_MSG_Q_LEN];

  /* Wait for door bell and hand-over controller out queue */
  {
    osSemaphoreWait(c2uartRx_BSemHandle, osWaitForever);
    msgLen = controllerMsgPullFromOutQueue(msgAry, Destinations__Network_UartRx, 1UL);                // Special case of callbacks need to limit blocking time
  }

  /* Decode and execute the commands when a message exists
   * (in case of callbacks the loop catches its wakeup semaphore
   * before ctrlQout is released results to request on an empty queue) */
  if (msgLen) {
    uartRxMsgProcess(msgLen, msgAry);
  }
}
