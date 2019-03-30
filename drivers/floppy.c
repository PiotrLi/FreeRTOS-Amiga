#include <FreeRTOS/FreeRTOS.h>
#include <FreeRTOS/queue.h>

#include <interrupt.h>
#include <custom.h>
#include <cia.h>

#include <stdint.h>
#include <stdio.h>

#include <floppy.h>

#define DEBUG 0

#define FLOPPYIO_MAXNUM 8

typedef struct FloppyIO {
  xTaskHandle origin;  /* task waiting for this track to become available */
  void *track;         /* chip memory buffer */
  uint16_t trackNum;   /* track number to transfer */
} FloppyIO_t;

static xTaskHandle FloppyIOTask;
static QueueHandle_t FloppyIOQueue;
static void FloppyIOThread(void *);

static ISR(TrackTransferDone) {
  /* Signal end of interrupt. */
  ClearIRQ(INTF_DSKBLK);

  /* Send notification to waiting task. */
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(FloppyIOTask, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void FloppyReader(void *);

void FloppyInit(unsigned aFloppyIOTaskPrio) {
  printf("[Init] Floppy drive driver!\n");

  int timer = AcquireTimer(TIMER_CIAA_A);
  configASSERT(timer != -1);

  /* Set standard synchronization marker. */
  custom->dsksync = DSK_SYNC;

  /* Standard settings for Amiga format disk floppies. */
  custom->adkcon = ADKF_SETCLR | ADKF_MFMPREC | ADKF_WORDSYNC | ADKF_FAST;

  /* Handler that will wake up track reader task. */
  SetIntVec(DSKBLK, TrackTransferDone);

  FloppyIOQueue = xQueueCreate(FLOPPYIO_MAXNUM, sizeof(FloppyIO_t));
  configASSERT(FloppyIOQueue != NULL);

  xTaskCreate(FloppyReader, "FloppyReader", configMINIMAL_STACK_SIZE, NULL,
              aFloppyIOTaskPrio, &FloppyIOTask);
  configASSERT(FloppyIOTask != NULL);
}

static void FloppyMotorOff(void);

void FloppyKill(void) {
  DisableINT(INTF_DSKBLK);
  DisableDMA(DMAF_DISK);
  ResetIntVec(DSKBLK);
  FloppyMotorOff();

  vTaskDelete(FloppyIOTask);
  vQueueDelete(FloppyIOQueue);
}

/******************************************************************************/

#define LOWER 0
#define UPPER 1

#define OUTWARDS 0
#define INWARDS 1

static int16_t MotorOn;
static int16_t HeadDir;
static int16_t TrackNum;

#define STEP_SETTLE TIMER_MS(3)

static void StepHeads(void) {
  uint8_t *ciaprb = (uint8_t *)&ciab->ciaprb;

  BCLR(ciaprb, CIAB_DSKSTEP);
  BSET(ciaprb, CIAB_DSKSTEP);

  WaitTimer(TIMER_CIAA_A, STEP_SETTLE);

  TrackNum += HeadDir;
}

#define DIRECTION_REVERSE_SETTLE TIMER_MS(18)

static inline void HeadsStepDirection(int16_t inwards) {
  uint8_t *ciaprb = (uint8_t *)&ciab->ciaprb;

  if (inwards) {
    BCLR(ciaprb, CIAB_DSKDIREC);
    HeadDir = 2;
  } else {
    BSET(ciaprb, CIAB_DSKDIREC);
    HeadDir = -2;
  }

  WaitTimer(TIMER_CIAA_A, DIRECTION_REVERSE_SETTLE);
}

static inline void ChangeDiskSide(int16_t upper) {
  uint8_t *ciaprb = (uint8_t *)&ciab->ciaprb;

  if (upper) {
    BCLR(ciaprb, CIAB_DSKSIDE);
    TrackNum++;
  } else {
    BSET(ciaprb, CIAB_DSKSIDE);
    TrackNum--;
  }
}

static inline void WaitDiskReady(void) {
  while (ciaa->ciapra & CIAF_DSKRDY);
}

static inline int HeadsAtTrack0() {
  return !(ciaa->ciapra & CIAF_DSKTRACK0);
}

static void FloppyMotorOn(void) {
  if (MotorOn)
    return;

  uint8_t *ciaprb = (uint8_t *)&ciab->ciaprb;

  BSET(ciaprb, CIAB_DSKSEL0);
  BCLR(ciaprb, CIAB_DSKMOTOR);
  BCLR(ciaprb, CIAB_DSKSEL0);

  WaitDiskReady();

  MotorOn = 1;
}

static void FloppyMotorOff(void) {
  if (!MotorOn)
    return;

  uint8_t *ciaprb = (uint8_t *)&ciab->ciaprb;

  BSET(ciaprb, CIAB_DSKSEL0);
  BSET(ciaprb, CIAB_DSKMOTOR);
  BCLR(ciaprb, CIAB_DSKSEL0);

  MotorOn = 0;
}

#define DISK_SETTLE TIMER_MS(15)

static void FloppyReader(void *) {
  /* Move head to track 0 */
  FloppyMotorOn();
  HeadsStepDirection(OUTWARDS);
  while (!HeadsAtTrack0())
    StepHeads();
  HeadsStepDirection(INWARDS);
  ChangeDiskSide(LOWER);
  TrackNum = 0;

  for (;;) {
    FloppyIO_t fio;

    if (xQueueReceive(FloppyIOQueue, &fio, 1000 / portTICK_PERIOD_MS)) {
      /* Turn the motor on. */
      FloppyMotorOn();

      /* Switch heads if needed. */
      if ((fio.trackNum ^ TrackNum) & 1)
        ChangeDiskSide(fio.trackNum & 1);

      /* Travel to requested track. */
      if (fio.trackNum != TrackNum) {
        HeadsStepDirection(fio.trackNum > TrackNum);
        while (fio.trackNum != TrackNum)
          StepHeads();
      }

      /* Wait for the head to stabilize over the track. */
      WaitTimer(TIMER_CIAA_A, DISK_SETTLE);

      /* Make sure the DMA for the disk is turned off. */
      custom->dsklen = 0;

#if DEBUG
      printf("[Floppy] Read track %d.\n", (int)fio.trackNum);
#endif

      /* Prepare for transfer. */
      ClearIRQ(INTF_DSKBLK);
      EnableINT(INTF_DSKBLK);
      EnableDMA(DMAF_DISK);

      /* Buffer in chip memory. */
      custom->dskpt = (void *)fio.track;

      /* Write track size twice to initiate DMA transfer. */
      custom->dsklen = DSK_DMAEN | (TRACK_SIZE / sizeof(int16_t));
      custom->dsklen = DSK_DMAEN | (TRACK_SIZE / sizeof(int16_t));

      (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

      /* Disable DMA & interrupts. */
      custom->dsklen = 0;
      DisableINT(INTF_DSKBLK);
      DisableDMA(DMAF_DISK);

      /* Wake up the task that requested transfer. */
      xTaskNotifyGive(fio.origin);
    } else {
      FloppyMotorOff();
    }
  }
}

void ReadFloppyTrack(void *aTrack, uint16_t aTrackNum) {
  FloppyIO_t fio = {.origin = xTaskGetCurrentTaskHandle(),
                    .track = aTrack,
                    .trackNum = aTrackNum};

  (void)xQueueSend(FloppyIOQueue, (void *)&fio, portMAX_DELAY);
  (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
