Setup: write /dev/serial-48024000 (UART2 TX, P9_21) -> wire ->
P9_11 (UART4 RX) -> IRQ on 481a8000. FIFOs disabled (1-char RHR/THR).

## I1 - register the handler (stub)
- platform_get_irq() -> virtual IRQ; DT has the INTC hwirq
  /proc/interrupts: 46 = INTC 74 (UART2), 47 = INTC 45 (UART4), Level
- Order: devm_request_irq() BEFORE enabling UART_IER_RDI
- err_irq path disables IER; remove() disables IER before pm put
  (devm free_irq runs only after remove() returns)
- Stub without ack -> level line stays asserted -> flood
  (10000 calls in ~140 ms, ~14 us each; flood guard disables IER)
- IRQ_HANDLED tells the KERNEL; only a register access acks the HARDWARE
- "callbacks suppressed" is printed lazily, on the next allowed message

## I2 - ack by reading UART_RX
- Read LSR first (snapshot, clears OE), then RX (= ack, DR -> 0)
- 1 IRQ per char, LSR 0x61 -> after ack 0x60
- No overrun despite ~6 ms printk in handler: sender is on the same
  single CPU, cannot refill THR while the handler runs (self-throttled)
- virq mapping persists after free_irq; /proc/interrupts count lives in
  irq_desc, survives rmmod (only reboot resets it)

## I3 - circular buffer + wait queue + read()
- Handler = producer (no printk), read() = consumer, rd == wr = empty
- init_waitqueue_head() BEFORE request_irq (handler may run at once)
- wait_event_interruptible(): macro; sleeps TASK_INTERRUPTIBLE,
  re-checks condition on each wake_up, -ERESTARTSYS on signal
  (never seen by user space: process killed, SA_RESTART, or EINTR)
- wchan = serial_read (macro inlined, schedule() skipped)
- read() returns 1: short reads are legal, cat loops
- wake_up() only makes the reader runnable; it runs when scheduled
- wait_event() (uninterruptible) -> D state, kill does not work

## I4 - where data gets lost (review-only experiments)
- A: lab design wraps when 16 chars arrive -> buffer looks EMPTY,
  whole buffer lost silently; drop_full keeps 15 (one slot always empty)
- B: non-preemptible kernel + busy-polling writer -> reader never runs
  during write() (104 ms!). cond_resched() only yields if need_resched
  is already set; EEVDF lets the writer finish its slice (38 chars:
  0 yields, nice 19 no help). Long write: 194 yields, still 44% lost.
  -> correctness must not depend on scheduling: bigger buffer,
     flow control, TX interrupt + sleeping writer
- C: handler latency 300 us -> next char hits a full RHR -> OE,
  "abcdef" -> "ace": AM335x keeps the OLD char
- Fixes for HW overrun: RX FIFO, ack early, keep IRQs-off sections short

## Open questions
- B5/B6: 1 hw_overrun per ~104 ms with latency_us=0 -> some IRQs-off
  section > 87 us somewhere: find it with ftrace irqsoff (debugging lab)
- Exact EEVDF slice on this board: /sys/kernel/debug/sched/base_slice_ns
- UART4 received while parent target-module suspended (ti-sysc) - from
  the I/O memory review
