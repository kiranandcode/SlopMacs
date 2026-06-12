;;; webvterm-batch-test.el --- batch smoke test for web-vterm-* primitives -*- lexical-binding: t -*-

;; Run: ./src/emacs -Q --batch -l test/webvterm-batch-test.el

(defvar wvt-failures 0)

(defmacro wvt-check (desc form)
  `(let ((res (condition-case err ,form (error (list 'ERROR err)))))
     (if (eq res t)
         (message "PASS %s" ,desc)
       (setq wvt-failures (1+ wvt-failures))
       (message "FAIL %s => %S" ,desc res))))

(wvt-check "primitives exist"
  (and (fboundp 'web-vterm-new) (fboundp 'web-vterm-write)
       (fboundp 'web-vterm-update) (fboundp 'web-vterm-key-input)
       t))

(let ((h (web-vterm-new 5 20))
      (buf (generate-new-buffer "*wvt*")))

  (wvt-check "handle is integer" (integerp h))

  ;; Plain text
  (web-vterm-write h "hello")
  (web-vterm-update h buf)
  (wvt-check "plain text rendered"
    (with-current-buffer buf
      (equal "hello" (buffer-substring-no-properties 1 6))))

  (wvt-check "buffer has 5 rows"
    (with-current-buffer buf (eq (count-lines (point-min) (point-max)) 5)))

  (wvt-check "cursor after hello"
    (equal (web-vterm-get-cursor h) '(0 . 5)))

  ;; CR/LF and cursor movement
  (web-vterm-write h "\r\nworld")
  (web-vterm-update h buf)
  (wvt-check "second line"
    (with-current-buffer buf
      (goto-char (point-min)) (forward-line 1)
      (equal "world" (buffer-substring-no-properties (point) (+ (point) 5)))))

  ;; Colors: red foreground via SGR
  (web-vterm-write h "\e[31mRED\e[0m")
  (web-vterm-update h buf)
  (wvt-check "red face applied"
    (with-current-buffer buf
      (goto-char (point-min)) (forward-line 1)
      (let ((face (get-text-property (+ (point) 5) 'face)))
        (and (plist-get face :foreground) t))))

  ;; Bold
  (web-vterm-write h "\e[1mB\e[0m")
  (web-vterm-update h buf)
  (wvt-check "bold face applied"
    (with-current-buffer buf
      (goto-char (point-min)) (forward-line 1)
      (let ((face (get-text-property (+ (point) 8) 'face)))
        (eq (plist-get face :weight) 'bold))))

  ;; Key encoding
  (wvt-check "return key encodes CR"
    (equal (web-vterm-key-input h 'return 0) "\r"))
  (wvt-check "up arrow encodes CSI A"
    (equal (web-vterm-key-input h 'up 0) "\e[A"))
  (wvt-check "C-c encodes ETX"
    (equal (web-vterm-key-input h ?c 4) "\C-c"))
  (wvt-check "plain char passes through"
    (equal (web-vterm-key-input h ?x 0) "x"))

  ;; Title via OSC
  (web-vterm-write h "\e]2;my-title\e\\")
  (wvt-check "title set" (equal (web-vterm-get-title h) "my-title"))

  ;; Resize
  (web-vterm-resize h 10 40)
  (web-vterm-update h buf)
  (wvt-check "resize to 10 rows"
    (with-current-buffer buf (eq (count-lines (point-min) (point-max)) 10)))

  ;; Device attributes query generates pending output
  (web-vterm-write h "\e[c")
  (wvt-check "DA query produces output"
    (> (length (web-vterm-get-output h)) 0))

  ;; Alternate screen (what full-screen TUIs use)
  (web-vterm-write h "\e[?1049h\e[2J\e[HALT")
  (web-vterm-update h buf)
  (wvt-check "altscreen renders"
    (with-current-buffer buf
      (equal "ALT" (buffer-substring-no-properties 1 4))))
  (web-vterm-write h "\e[?1049l")
  (web-vterm-update h buf)
  (wvt-check "main screen restored"
    (with-current-buffer buf
      (goto-char (point-min)) (forward-line 1)
      (equal "world" (buffer-substring-no-properties (point) (+ (point) 5)))))

  ;; Wide chars
  (web-vterm-write h "\r\n\e[K日本")
  (web-vterm-update h buf)
  (wvt-check "wide chars render"
    (with-current-buffer buf
      (goto-char (point-min)) (forward-line 2)
      (equal "日本" (buffer-substring-no-properties (point) (+ (point) 2)))))

  (web-vterm-destroy h)
  (wvt-check "destroyed handle rejected"
    (condition-case nil (progn (web-vterm-write h "x") nil) (error t)))

  (kill-buffer buf))

(if (> wvt-failures 0)
    (progn (message "%d FAILURES" wvt-failures) (kill-emacs 1))
  (message "ALL PASS"))
