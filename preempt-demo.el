;;; preempt-demo.el --- feel preemptive threads interactively -*- lexical-binding: t -*-

;; Load this file, then:
;;
;;   M-x preempt-demo        - 10 seconds of CPU-bound elisp in a thread.
;;                             Keep typing in any buffer; progress is
;;                             reported in *preempt-demo*.
;;   M-x preempt-demo-regex  - ~6 seconds inside a SINGLE C primitive
;;                             (catastrophic regex backtracking).
;;
;; Compare with (setq thread-preemption nil), where either command
;; freezes the editor solid until it finishes.

(defun preempt-demo ()
  "Run a 10-second byte-compiled busy loop in a background thread."
  (interactive)
  (let ((buf (get-buffer-create "*preempt-demo*"))
        (fn (byte-compile
             '(lambda (report)
                (let ((start (float-time)) (i 0))
                  (while (< (- (float-time) start) 10.0)
                    (setq i (1+ i))
                    (when (zerop (% i 5000000))
                      (funcall report i (- (float-time) start))))
                  (funcall report i (- (float-time) start)))))))
    (pop-to-buffer buf)
    (goto-char (point-max))
    (insert "\n--- busy thread started; keep typing elsewhere ---\n")
    (make-thread
     (lambda ()
       (funcall fn (lambda (i elapsed)
                     (with-current-buffer buf
                       (goto-char (point-max))
                       (insert (format "busy: %dM iterations, %.1fs elapsed\n"
                                       (/ i 1000000) elapsed)))))
       (with-current-buffer buf
         (goto-char (point-max))
         (insert "--- busy thread finished ---\n")))
     "preempt-demo")))

(defun preempt-demo-regex ()
  "Run one ~6-second C primitive (regex backtracking) in a thread."
  (interactive)
  (let ((buf (get-buffer-create "*preempt-demo*")))
    (pop-to-buffer buf)
    (goto-char (point-max))
    (insert "\n--- single string-match call started; keep typing ---\n")
    (make-thread
     (lambda ()
       (let ((start (float-time)))
         (string-match "\\(a+\\)+b" (make-string 27 ?a))
         (with-current-buffer buf
           (goto-char (point-max))
           (insert (format "--- string-match returned after %.1fs ---\n"
                           (- (float-time) start))))))
     "preempt-demo-regex")))

(provide 'preempt-demo)
;;; preempt-demo.el ends here
