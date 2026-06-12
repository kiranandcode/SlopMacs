;;; preempt-test.el --- validate preemptive thread time-slicing -*- lexical-binding: t -*-

;; Each test starts a CPU-bound thread, then counts how many times the
;; main thread gets scheduled (one "beat" per 10ms sleep) while the
;; busy thread is still running.  Without preemption the busy thread
;; never releases the global lock, so the main thread blocks until it
;; finishes: beats stays at ~1.  With preemption, beats should track
;; elapsed-time / ~25ms.

(defun preempt-test--run (label make-busy-thread)
  (let ((done nil) (beats 0) (start (float-time)) (first-beat nil))
    (funcall make-busy-thread (lambda () (setq done t)))
    (while (and (not done) (< (- (float-time) start) 60))
      (sleep-for 0.01)
      (unless done
        (setq beats (1+ beats))
        (unless first-beat (setq first-beat (- (float-time) start)))))
    (message "RESULT %s: beats=%d first-beat=%.3fs elapsed=%.2fs done=%s"
             label beats (or first-beat -1) (- (float-time) start) done)
    beats))

(message "thread-preemption=%s thread-slice-ms=%d" thread-preemption thread-slice-ms)

;; Test 0: baseline with preemption disabled — expect starvation (beats ~0-1).
(setq thread-preemption nil)
(let ((fn (byte-compile '(lambda ()
                           (let ((i 0)) (while (< i 20000000) (setq i (1+ i))))))))
  (preempt-test--run "baseline-no-preempt"
                     (lambda (finish)
                       (make-thread (lambda () (funcall fn) (funcall finish)) "busy0"))))

;; Test 1: byte-compiled elisp busy loop, preemption on.
(setq thread-preemption t)
(let ((fn (byte-compile '(lambda ()
                           (let ((i 0)) (while (< i 60000000) (setq i (1+ i))))))))
  (preempt-test--run "elisp-loop"
                     (lambda (finish)
                       (make-thread (lambda () (funcall fn) (funcall finish)) "busy1"))))

;; Test 2: one long-running C primitive — catastrophic regex backtracking.
;; The entire computation is a single call to string-match; the only
;; preemption opportunities are the maybe_quit calls inside
;; regex-emacs.c's match loop.
(preempt-test--run "c-primitive-regex"
                   (lambda (finish)
                     (make-thread (lambda ()
                                    (string-match "\\(a+\\)+b" (make-string 27 ?a))
                                    (funcall finish))
                                  "busy2")))

;; Test 3: interpreted elisp (funcall/eval path rather than bytecode).
(let ((fn '(lambda ()
             (let ((i 0)) (while (< i 3000000) (setq i (1+ i)))))))
  (preempt-test--run "interpreted-loop"
                     (lambda (finish)
                       (make-thread (lambda () (funcall fn) (funcall finish)) "busy3"))))

;; Test 4: GC safety — main thread allocates heavily (forcing GCs)
;; while the busy thread is repeatedly parked mid-C-primitive holding
;; string-data pointers.  Exercises the compact_small_strings guard.
(let ((done nil) (start (float-time)) (gcs gcs-done))
  (make-thread (lambda ()
                 (string-match "\\(a+\\)+b" (make-string 26 ?a))
                 (setq done t))
               "busy4")
  (while (and (not done) (< (- (float-time) start) 60))
    (let ((junk (make-list 1000 (make-string 8 ?x))))
      (ignore junk))
    (garbage-collect))
  (message "RESULT gc-stress: survived=%s gcs=%d elapsed=%.2fs"
           done (- gcs-done gcs) (- (float-time) start)))

(message "ALL TESTS COMPLETE")
