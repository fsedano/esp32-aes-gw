/**
  ******************************************************************************
  * @file    comm_task.h
  * @brief   Control (TCP :5000) + stream (UDP :10737) transport, driving the
  *          lccore protocol dispatcher.
  ******************************************************************************
  */

#ifndef MAIN_COMM_TASK_H
#define MAIN_COMM_TASK_H

/* Spawn the comm task. identity_init() and lc_log_init() must have run. */
void comm_start(void);

#endif /* MAIN_COMM_TASK_H */
