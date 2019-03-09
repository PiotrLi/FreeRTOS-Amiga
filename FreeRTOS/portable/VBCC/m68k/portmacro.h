#ifndef PORTMACRO_H
#define PORTMACRO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Type definitions. */
#define portCHAR char
#define portFLOAT float
#define portDOUBLE double
#define portLONG long
#define portSHORT short
#define portSTACK_TYPE uint32_t
#define portBASE_TYPE long

typedef portSTACK_TYPE StackType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;

#if (configUSE_16_BIT_TICKS == 1)
typedef uint16_t TickType_t;
#define portMAX_DELAY (TickType_t)0xffff
#else
typedef uint32_t TickType_t;
#define portMAX_DELAY (TickType_t)0xffffffffUL
#endif

/* VBCC does not understand common way to mark unused variables,
 * i.e. "(void)x", and it complains about efectless statement.
 * Following macros invocations silence out those warnings. */
#define portUNUSED(x)
#define portSETUP_TCB(pxTCB)
#define portCLEAN_UP_TCB(pxTCB)

/* Hardware specifics. */
#define portBYTE_ALIGNMENT 4
#define portSTACK_GROWTH -1
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)

/* When code executes in task context, disabling and enabling interrupts is
 * trivial with use of INTENA register. Just clear / set master bit. */
void portDISABLE_INTERRUPTS() = "\tmove.w\t#$4000,$dff09a\n";
void portENABLE_INTERRUPTS() = "\tmove.w\t#$c000,$dff09a\n";

/* Functions that enter/exit critical section protecting against interrupts. */
void vPortEnterCritical(void);
void vPortExitCritical(void);

#define portENTER_CRITICAL() vPortEnterCritical()
#define portEXIT_CRITICAL() vPortExitCritical()

/* When code executes in ISR context it may be interrupted on M68000 by higher
 * priority level interrupt. To construct critical section we need to
 * use mask bits in SR register. */
int ulPortSetInterruptMaskFromISR(void);
void vPortClearInterruptMaskFromISR(__reg("d0") int uxSavedStatusRegister);

#define portSET_INTERRUPT_MASK_FROM_ISR(void) ulPortSetInterruptMaskFromISR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(uxSavedStatusRegister)               \
  vPortClearInterruptMaskFromISR(uxSavedStatusRegister)

/* When simulator is configured to enter debugger on illegal instructions,
 * this macro can be used to set breakpoints in your code. */
void portBREAK() = "\tillegal\n";

/* Following procedure is used to yield CPU time. */
extern void vPortYield(void);

#define portYIELD() vPortYield()
#define portEND_SWITCHING_ISR(xSwitchRequired)                                 \
  if (xSwitchRequired) {                                                       \
    portYIELD();                                                               \
  }
#define portYIELD_FROM_ISR(x) portEND_SWITCHING_ISR(x)

/* Task function macros as described on the FreeRTOS.org WEB site. */
#define portTASK_FUNCTION_PROTO(vFunction, pvParameters)                       \
  void vFunction(void *pvParameters)
#define portTASK_FUNCTION(vFunction, pvParameters)                             \
  void vFunction(void *pvParameters)

#ifdef __cplusplus
}
#endif

#endif /* PORTMACRO_H */
