;;; web-parallel.el --- Parallel evaluation using Guile threads -*- lexical-binding: t -*-

;; Copyright (C) 2026 Free Software Foundation, Inc.

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; High-level parallel evaluation API built on Guile's threading support.
;; Guile has no GIL, so threads run truly in parallel on multiple CPU cores.
;;
;; Main entry points:
;;   `web-future'          -- start an async computation
;;   `web-future-get'      -- wait for and retrieve the result
;;   `web-parallel-map'    -- parallel map across a list
;;   `web-parallel-let'    -- evaluate bindings in parallel
;;   `web-parallel-async'  -- fire-and-forget with optional callback

;;; Code:

;;;; Futures

(defun web-future (thunk)
  "Start THUNK in a parallel Guile thread, returning a future object.
THUNK should be a zero-argument function.  The returned future can be
passed to `web-future-get' to retrieve the result.

Example:
  (let ((f (web-future (lambda () (+ 1 2)))))
    (web-future-get f))  ;; => 3"
  (let* ((result nil)
         (done nil)
         (thread (guile-spawn-thread
                  (lambda ()
                    (setq result (funcall thunk))
                    (setq done t)
                    result))))
    (list 'web-future thread #'(lambda () result) #'(lambda () done))))

(defun web-future-get (future)
  "Wait for FUTURE to complete and return its result.
Blocks until the thread finishes."
  (unless (and (listp future) (eq (car future) 'web-future))
    (error "Not a web-future: %S" future))
  (guile-thread-join (nth 1 future))
  (funcall (nth 2 future)))

(defun web-future-done-p (future)
  "Return non-nil if FUTURE has completed."
  (unless (and (listp future) (eq (car future) 'web-future))
    (error "Not a web-future: %S" future))
  (funcall (nth 3 future)))

;;;; Parallel map

(defun web-parallel-map (fn list)
  "Apply FN to each element of LIST in parallel, returning results.
Each application runs in its own Guile thread.  Results are returned
in the same order as the input LIST.

Example:
  (web-parallel-map (lambda (x) (* x x)) '(1 2 3 4))
  ;; => (1 4 9 16)  -- computed in parallel"
  (let ((futures (mapcar (lambda (item)
                           (web-future (lambda () (funcall fn item))))
                         list)))
    (mapcar #'web-future-get futures)))

;;;; Parallel let

(defmacro web-parallel-let (bindings &rest body)
  "Evaluate BINDINGS in parallel, then execute BODY.
Each binding is (VAR EXPR).  All EXPR forms are evaluated concurrently
in separate Guile threads.

Example:
  (web-parallel-let ((a (expensive-computation-1))
                     (b (expensive-computation-2)))
    (+ a b))"
  (declare (indent 1))
  (let ((future-vars (mapcar (lambda (b) (make-symbol (concat "f-" (symbol-name (car b)))))
                             bindings)))
    `(let (,@(cl-mapcar (lambda (fvar binding)
                          `(,fvar (web-future (lambda () ,(cadr binding)))))
                        future-vars bindings))
       (let (,@(cl-mapcar (lambda (binding fvar)
                            `(,(car binding) (web-future-get ,fvar)))
                          bindings future-vars))
         ,@body))))

;;;; Fire and forget

(defun web-parallel-async (thunk &optional callback)
  "Run THUNK in a parallel thread.
If CALLBACK is non-nil, it is called with the result when THUNK completes.
Returns the thread object."
  (guile-spawn-thread
   (lambda ()
     (let ((result (funcall thunk)))
       (when callback
         (funcall callback result))
       result))))

(provide 'web-parallel)

;;; web-parallel.el ends here
