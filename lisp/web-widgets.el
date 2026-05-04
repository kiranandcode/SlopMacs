;;; web-widgets.el --- Web applet widgets for the Emacs web display -*- lexical-binding: t -*-

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

;; Web applet widgets that live inside Emacs buffers.
;;
;; This piggybacks on the IMAGE_GLYPH pipeline: a transparent SVG
;; placeholder image (with a data-widget marker) is inserted via
;; `insert-image'.  The browser detects the marker, and instead of
;; drawing the static image on the canvas, it creates an interactive
;; HTML overlay at the image's exact position.  As the buffer scrolls,
;; the image glyph moves and the overlay follows automatically.
;;
;; User API:
;;
;;   (web-widget-insert ID WIDTH HEIGHT JS-CODE)
;;
;; JS-CODE receives `el' (the overlay container element) and can
;; render anything — D3 charts, forms, canvases, iframes, etc.
;;
;; Example:
;;
;;   (web-widget-insert "clock" 300 60
;;     "el.style.cssText='color:#fff;font:24px monospace;padding:8px';
;;      setInterval(function(){el.textContent=new Date().toLocaleTimeString()},1000);")

;;; Code:

(defgroup web-widgets nil
  "Web applet widgets for the Emacs web display."
  :group 'web)

(defvar web-widget-d3-url
  "https://cdn.jsdelivr.net/npm/d3@7/dist/d3.min.js"
  "URL to load D3.js from.")

(defvar web-widget--counter 0
  "Counter to make each widget SVG placeholder unique.")

(defun web-widget--ensure-web ()
  "Signal an error unless the web display is available."
  (unless (fboundp 'web-eval-javascript)
    (user-error "Web display not available")))

;; -----------------------------------------------------------------
;;  Core: insert a widget using the image pipeline
;; -----------------------------------------------------------------

(defun web-widget-insert (id width height js-code)
  "Insert a web widget into the current buffer at point.
ID is a unique string identifier.  WIDTH and HEIGHT are pixel
dimensions.  JS-CODE is JavaScript that receives `el' (the
overlay DOM element) and renders content into it.

The widget scrolls with buffer text, just like an image."
  (web-widget--ensure-web)
  ;; Create a transparent SVG placeholder with the widget marker.
  ;; Each SVG must be unique (different data) to prevent Emacs's
  ;; image cache from deduplicating them.  A nonce and unique text
  ;; element ensure distinct image specs.
  (let* ((nonce (cl-incf web-widget--counter))
         (svg-data (format "<svg xmlns='http://www.w3.org/2000/svg' \
width='%d' height='%d' data-widget=\"%s\" data-n=\"%d\">\
<rect width='100%%' height='100%%' fill='rgba(0,0,0,0.002)'/>\
<text x='-9999' y='-9999'>%s %d</text></svg>"
                           width height id nonce id nonce))
         (img (create-image svg-data 'svg t
                            :width width :height height
                            :ascent 'center)))
    ;; Insert the image as inline content.
    (insert-image img (format "[widget:%s]" id)))
  ;; Register the widget renderer on the browser side.
  (web-eval-javascript
   (format "_widgets.registerWidget('%s', function(el){%s});"
           id js-code)))

;; -----------------------------------------------------------------
;;  D3.js Chart Demo
;; -----------------------------------------------------------------

(defconst web-widget--line-chart-js "
_widgets.loadScript('%s').then(function(){
  var d3=window.d3, W=el.offsetWidth, H=el.offsetHeight;
  el.style.background='#1e1e32';el.style.borderRadius='8px';
  el.style.boxShadow='0 2px 12px rgba(0,0,0,0.4)';
  var C={accent:'#7c6af6',text:'#abb2bf',dim:'rgba(171,178,191,0.5)',grid:'rgba(255,255,255,0.06)'};
  var data=[],v=50,now=Date.now();
  for(var i=0;i<50;i++){v+=(Math.random()-0.48)*8;v=Math.max(10,Math.min(100,v));data.push({t:new Date(now-(50-i)*60000),v:v});}
  var m={t:12,r:16,b:28,l:44},w=W-m.l-m.r,h=H-m.t-m.b;
  var svg=d3.select(el).append('svg').attr('width',W).attr('height',H);
  var g=svg.append('g').attr('transform','translate('+m.l+','+m.t+')');
  var x=d3.scaleTime().domain(d3.extent(data,function(d){return d.t})).range([0,w]);
  var y=d3.scaleLinear().domain([0,d3.max(data,function(d){return d.v})*1.1]).range([h,0]);
  g.append('g').selectAll('line').data(y.ticks(5)).join('line')
    .attr('x1',0).attr('x2',w).attr('y1',function(d){return y(d)}).attr('y2',function(d){return y(d)}).attr('stroke',C.grid);
  var gid='lg'+Math.random().toString(36).slice(2);
  var grad=svg.append('defs').append('linearGradient').attr('id',gid).attr('x1',0).attr('y1',0).attr('x2',0).attr('y2',1);
  grad.append('stop').attr('offset','0%%').attr('stop-color',C.accent).attr('stop-opacity',0.4);
  grad.append('stop').attr('offset','100%%').attr('stop-color',C.accent).attr('stop-opacity',0);
  g.append('path').datum(data).attr('fill','url(#'+gid+')')
    .attr('d',d3.area().x(function(d){return x(d.t)}).y0(h).y1(function(d){return y(d.v)}).curve(d3.curveCatmullRom));
  var line=d3.line().x(function(d){return x(d.t)}).y(function(d){return y(d.v)}).curve(d3.curveCatmullRom);
  var path=g.append('path').datum(data).attr('fill','none').attr('stroke',C.accent).attr('stroke-width',2.5).attr('d',line);
  var len=path.node().getTotalLength();
  path.attr('stroke-dasharray',len).attr('stroke-dashoffset',len)
    .transition().duration(1500).ease(d3.easeCubicOut).attr('stroke-dashoffset',0);
  g.selectAll('.dot').data(data.filter(function(_,i){return i%%5===0})).join('circle')
    .attr('cx',function(d){return x(d.t)}).attr('cy',function(d){return y(d.v)}).attr('r',0)
    .attr('fill',C.accent).attr('stroke','#1e1e32').attr('stroke-width',2)
    .transition().delay(function(_,i){return 1200+i*80}).duration(300).attr('r',4);
  g.append('g').attr('transform','translate(0,'+h+')')
    .call(d3.axisBottom(x).ticks(5).tickFormat(d3.timeFormat('%%H:%%M')))
    .selectAll('text,line,path').attr('stroke',C.dim).attr('fill',C.dim);
  g.append('g').call(d3.axisLeft(y).ticks(5))
    .selectAll('text,line,path').attr('stroke',C.dim).attr('fill',C.dim);
  svg.append('text').attr('x',m.l).attr('y',10).attr('fill',C.text)
    .attr('font-size','12px').attr('font-weight','600').text('System Load (60m)');
});")

(defconst web-widget--bar-chart-js "
_widgets.loadScript('%s').then(function(){
  var d3=window.d3, W=el.offsetWidth, H=el.offsetHeight;
  el.style.background='#1e1e32';el.style.borderRadius='8px';
  el.style.boxShadow='0 2px 12px rgba(0,0,0,0.4)';
  var C={text:'#abb2bf',dim:'rgba(171,178,191,0.5)',grid:'rgba(255,255,255,0.06)'};
  var PAL=['#7c6af6','#e06c75','#98c379','#61afef','#c678dd','#d19a66'];
  var labels=['Lisp','C','Python','Rust','Go','JS','Haskell','OCaml'];
  var data=labels.map(function(l){return{l:l,v:20+Math.floor(Math.random()*80)}});
  var m={t:12,r:16,b:28,l:44},w=W-m.l-m.r,h=H-m.t-m.b;
  var svg=d3.select(el).append('svg').attr('width',W).attr('height',H);
  var g=svg.append('g').attr('transform','translate('+m.l+','+m.t+')');
  var x=d3.scaleBand().domain(data.map(function(d){return d.l})).range([0,w]).padding(0.3);
  var y=d3.scaleLinear().domain([0,d3.max(data,function(d){return d.v})*1.1]).range([h,0]);
  g.append('g').selectAll('line').data(y.ticks(5)).join('line')
    .attr('x1',0).attr('x2',w).attr('y1',function(d){return y(d)}).attr('y2',function(d){return y(d)}).attr('stroke',C.grid);
  g.selectAll('rect').data(data).join('rect')
    .attr('x',function(d){return x(d.l)}).attr('width',x.bandwidth()).attr('y',h).attr('height',0)
    .attr('rx',4).attr('fill',function(_,i){return PAL[i%%PAL.length]}).attr('opacity',0.85)
    .transition().delay(function(_,i){return i*80}).duration(600).ease(d3.easeCubicOut)
    .attr('y',function(d){return y(d.v)}).attr('height',function(d){return h-y(d.v)});
  g.selectAll('.vl').data(data).join('text')
    .attr('x',function(d){return x(d.l)+x.bandwidth()/2}).attr('y',function(d){return y(d.v)-6})
    .attr('text-anchor','middle').attr('fill',C.text).attr('font-size','11px')
    .attr('opacity',0).text(function(d){return d.v})
    .transition().delay(function(_,i){return 400+i*80}).duration(300).attr('opacity',1);
  g.append('g').attr('transform','translate(0,'+h+')').call(d3.axisBottom(x))
    .selectAll('text,line,path').attr('stroke',C.dim).attr('fill',C.dim).attr('font-size','11px');
  g.append('g').call(d3.axisLeft(y).ticks(5))
    .selectAll('text,line,path').attr('stroke',C.dim).attr('fill',C.dim);
  svg.append('text').attr('x',m.l).attr('y',10).attr('fill',C.text)
    .attr('font-size','12px').attr('font-weight','600').text('Language Usage');
});")

(defconst web-widget--donut-chart-js "
_widgets.loadScript('%s').then(function(){
  var d3=window.d3, W=el.offsetWidth, H=el.offsetHeight;
  el.style.background='#1e1e32';el.style.borderRadius='8px';
  el.style.boxShadow='0 2px 12px rgba(0,0,0,0.4)';
  var C={text:'#abb2bf',dim:'rgba(171,178,191,0.5)'};
  var PAL=['#7c6af6','#e06c75','#98c379','#61afef','#c678dd','#d19a66'];
  var data=[{l:'Editing',v:35},{l:'Compiling',v:20},{l:'Debugging',v:18},{l:'Reading',v:15},{l:'Coffee',v:12}];
  var r=Math.min(W,H)/2-20;
  var svg=d3.select(el).append('svg').attr('width',W).attr('height',H);
  var g=svg.append('g').attr('transform','translate('+W/2+','+H/2+')');
  var pie=d3.pie().value(function(d){return d.v}).sort(null).padAngle(0.03);
  var arc=d3.arc().innerRadius(r*0.55).outerRadius(r);
  var arcH=d3.arc().innerRadius(r*0.55).outerRadius(r+6);
  var arcs=g.selectAll('path').data(pie(data)).join('path')
    .attr('fill',function(_,i){return PAL[i%%PAL.length]}).attr('stroke','#1e1e32').attr('stroke-width',2)
    .style('cursor','pointer');
  arcs.transition().duration(800).ease(d3.easeCubicOut)
    .attrTween('d',function(d){var i=d3.interpolate({startAngle:0,endAngle:0},d);return function(t){return arc(i(t))}});
  arcs.on('mouseenter',function(ev,d){d3.select(this).transition().duration(150).attr('d',arcH(d))})
    .on('mouseleave',function(ev,d){d3.select(this).transition().duration(150).attr('d',arc(d))});
  var la=d3.arc().innerRadius(r*0.8).outerRadius(r*0.8);
  g.selectAll('text.lbl').data(pie(data)).join('text')
    .attr('transform',function(d){return'translate('+la.centroid(d)+')'}).attr('text-anchor','middle')
    .attr('fill','#fff').attr('font-size','11px').attr('font-weight','600')
    .attr('opacity',0).text(function(d){return d.data.l})
    .transition().delay(600).duration(300).attr('opacity',1);
  g.append('text').attr('text-anchor','middle').attr('dy','-0.2em')
    .attr('fill',C.text).attr('font-size','22px').attr('font-weight','700').text('100%%');
  g.append('text').attr('text-anchor','middle').attr('dy','1.2em')
    .attr('fill',C.dim).attr('font-size','11px').text('Productivity');
  svg.append('text').attr('x',14).attr('y',16).attr('fill',C.text)
    .attr('font-size','12px').attr('font-weight','600').text('Time Distribution');
});")

;;;###autoload
(defun web-charts-demo ()
  "Display a D3.js chart dashboard in an org-mode buffer.
The charts are interactive browser widgets that flow with
buffer text — scroll, navigate, and edit around them.

Press \\`q' to dismiss, \\`r' to refresh with new data."
  (interactive)
  (web-widget--ensure-web)
  (let ((buf (get-buffer "*Charts*")))
    (when buf
      (with-current-buffer buf
        (when (fboundp 'web-eval-javascript)
          (web-eval-javascript "_widgets.destroyAllOverlays();")))
      (kill-buffer buf)))
  (switch-to-buffer (get-buffer-create "*Charts*"))
  (org-mode)
  (let ((inhibit-read-only t)
        (chart-w 620)
        (chart-h 280))
    (erase-buffer)
    (insert "#+TITLE: Web Applets Dashboard\n\n")
    (insert "* Overview\n\n")
    (insert "  Interactive D3.js charts rendered by the browser, integrated\n")
    (insert "  into this org buffer via the image glyph pipeline.  Scroll!\n\n")
    (insert "  Press ~q~ to dismiss  |  ~r~ to regenerate data\n\n")
    (insert "* System Load\n\n")
    (web-widget-insert "line-chart" chart-w chart-h
                       (format web-widget--line-chart-js web-widget-d3-url))
    (insert "\n\n* Language Usage\n\n")
    (web-widget-insert "bar-chart" chart-w chart-h
                       (format web-widget--bar-chart-js web-widget-d3-url))
    (insert "\n\n* Time Distribution\n\n")
    (web-widget-insert "donut-chart" chart-w chart-h
                       (format web-widget--donut-chart-js web-widget-d3-url))
    (insert "\n\n* Notes\n\n")
    (insert "  Edit here.  The charts above are buffer content — they\n")
    (insert "  scroll, reflow, and behave like inline images.\n"))
  (goto-char (point-min))
  (setq buffer-read-only t)
  (use-local-map
   (let ((map (make-sparse-keymap)))
     (set-keymap-parent map (current-local-map))
     (define-key map "q" #'web-charts-dismiss)
     (define-key map "r" #'web-charts-refresh)
     map)))

;;;###autoload
(defun web-charts-dismiss ()
  "Dismiss charts and kill the buffer."
  (interactive)
  (when (fboundp 'web-eval-javascript)
    (web-eval-javascript "_widgets.destroyAllOverlays();"))
  (kill-buffer (current-buffer)))

;;;###autoload
(defun web-charts-refresh ()
  "Regenerate charts with new data."
  (interactive)
  (web-charts-demo))

(provide 'web-widgets)

;;; web-widgets.el ends here
