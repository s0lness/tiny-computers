/**
 * Local tldraw used as a handwriting capture rig.
 *
 * Written with React.createElement rather than JSX on purpose: Bun picks the
 * dev-vs-production JSX runtime from its own minify heuristic, which made the
 * page throw `jsxDEV is not a function` in one build mode and hid real errors
 * behind mangled names in the other. No JSX, no transform, no ambiguity.
 */

import { createElement as h, useCallback, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { Tldraw, type Editor } from "tldraw";
import "tldraw/tldraw.css";

const PANEL_W = 368;
const PANEL_H = 448;

type TimedPoint = { x: number; y: number; t: number };

function App() {
  const editorRef = useRef<Editor | null>(null);
  const traceRef = useRef<TimedPoint[][]>([]);
  const currentRef = useRef<TimedPoint[] | null>(null);
  const [status, setStatus] = useState("Draw inside the box, then press Send.");

  const onMount = useCallback((editor: Editor) => {
    editorRef.current = editor;
    editor.setCurrentTool("draw");

    // Record the raw pointer stream alongside whatever tldraw builds, so the
    // replay can run at the speed it was actually written.
    const down = (e: PointerEvent) => {
      if ((e.target as HTMLElement)?.closest?.(".capture-bar")) return;
      const p = editor.screenToPage({ x: e.clientX, y: e.clientY });
      currentRef.current = [{ x: p.x, y: p.y, t: performance.now() }];
    };
    const move = (e: PointerEvent) => {
      if (!currentRef.current) return;
      const p = editor.screenToPage({ x: e.clientX, y: e.clientY });
      currentRef.current.push({ x: p.x, y: p.y, t: performance.now() });
    };
    const up = () => {
      if (currentRef.current && currentRef.current.length > 1) {
        traceRef.current.push(currentRef.current);
      }
      currentRef.current = null;
    };

    window.addEventListener("pointerdown", down);
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
  }, []);

  const save = async () => {
    const editor = editorRef.current;
    if (!editor) return;

    const shapes = editor
      .getCurrentPageShapes()
      .filter((s: any) => s.type === "draw")
      .sort((a: any, b: any) => (a.index < b.index ? -1 : 1));

    const strokes = shapes.map((s: any) => ({
      x: s.x,
      y: s.y,
      isPen: !!s.props.isPen,
      isComplete: !!s.props.isComplete,
      size: s.props.size,
      points: s.props.segments.flatMap((seg: any) =>
        seg.points.map((p: any) => ({ x: p.x, y: p.y, z: p.z ?? 0.5 })),
      ),
    }));

    if (strokes.length === 0) {
      setStatus("Nothing drawn yet.");
      return;
    }

    const res = await fetch("/save", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        panel: { w: PANEL_W, h: PANEL_H },
        strokes,
        timing: traceRef.current.map((t) => ({
          ms: t[t.length - 1].t - t[0].t,
          points: t.length,
        })),
      }),
    });
    setStatus(res.ok ? `Saved: ${await res.text()}` : `Save failed: ${await res.text()}`);
  };

  const clear = () => {
    const editor = editorRef.current;
    if (!editor) return;
    editor.selectAll();
    editor.deleteShapes(editor.getSelectedShapeIds());
    traceRef.current = [];
    setStatus("Cleared.");
  };

  return h("div", null, [
    h("div", { className: "capture-bar", key: "bar" }, [
      h("button", { onClick: save, key: "s" }, "Send to device"),
      h("button", { onClick: clear, key: "c" }, "Clear"),
      h("span", { key: "t" }, status),
    ]),
    h("div", { className: "panel-guide", key: "guide" }),
    h("div", { style: { position: "fixed", inset: 0 }, key: "tl" }, h(Tldraw, { onMount })),
  ]);
}

createRoot(document.getElementById("root")!).render(h(App));
