// Maps a click/touch position in viewport (client) coordinates back to the
// panel's own, unrotated coordinate space, undoing whatever CSS rotation is
// currently applied to the puck on screen (a quick-rotate for devices used
// sideways, plus a cosmetic tilt slider on top of it).
//
// emu_abi.h is explicit that this is the emulator's job, not the browser's:
// "If a device is used rotated, mapping the pointer back is the emulator's
// job, because the firmware's own coordinate handling is under test and
// must not be helped." That is also why this does NOT lean on the browser
// inverting CSS transforms for us via PointerEvent.offsetX (which is
// spec-correct but happens implicitly, is easy to get backwards, and reads
// exactly like a firmware bug if the sign is wrong): the inverse rotation
// below is worked out explicitly so it can be read, checked, and tested as
// one small pure function.

export interface PanelPoint {
  x: number;
  y: number;
}

export interface MappedTouch {
  // What emu_touch actually gets: the panel's own, unrotated space.
  panel: PanelPoint;
  // The same click, expressed in whatever orientation the puck is
  // currently shown in on screen (its rendered width/height, swapped when
  // quick-rotated a quarter turn). Purely a readout: most apps on a device
  // used sideways have their own layout constants in that space, and
  // debugging against the wrong one wastes a round trip.
  view: PanelPoint;
  viewW: number;
  viewH: number;
}

function clampRound(v: number, max: number): number {
  return Math.max(0, Math.min(max - 1, Math.round(v)));
}

// quickDeg is the deliberate rotate control (0 / 90 / -90 / 180 only,
// enforced by the caller): a device is used rotated, or it isn't, and it
// stays whichever quarter-turn it is. tiltDeg is the small cosmetic slider
// on top, for a jauntier photo of the puck, and is folded into the panel
// mapping (so touch is still correct while tilted) but deliberately left
// out of the "view" space, which only means something at exact quarter
// turns.
export function mapClientPoint(
  clientX: number,
  clientY: number,
  canvas: HTMLElement,
  quickDeg: number,
  tiltDeg: number,
  panelW: number,
  panelH: number
): MappedTouch {
  const rect = canvas.getBoundingClientRect();
  // A CSS rotation's default transform-origin is the element's own center,
  // so the center of the (possibly larger, axis-aligned) post-rotation
  // bounding rect is still exactly the canvas's true on-screen center,
  // regardless of angle.
  const cx = rect.left + rect.width / 2;
  const cy = rect.top + rect.height / 2;
  const dx = clientX - cx;
  const dy = clientY - cy;

  const totalDeg = quickDeg + tiltDeg;
  const theta = (totalDeg * Math.PI) / 180;
  const cos = Math.cos(theta);
  const sin = Math.sin(theta);
  // Inverse of CSS's own rotation matrix, rotate(theta):
  //   screen = [x*cos - y*sin, x*sin + y*cos]
  // Its inverse (rotation matrices are orthogonal, so inverse = transpose):
  const localDx = dx * cos + dy * sin;
  const localDy = -dx * sin + dy * cos;

  // canvas.clientWidth/clientHeight are the element's own LAYOUT size,
  // which a CSS transform never changes (transforms are paint-only), so
  // this recovers the canvas's local, unrotated coordinate space.
  const cw = canvas.clientWidth || 1;
  const ch = canvas.clientHeight || 1;
  const panel: PanelPoint = {
    x: clampRound(((localDx + cw / 2) * panelW) / cw, panelW),
    y: clampRound(((localDy + ch / 2) * panelH) / ch, panelH),
  };

  // View space: the same click re-based into the rendered (quick-rotated
  // only) box, using the known unrotated cw/ch rather than the tilt-padded
  // getBoundingClientRect box, so tilt cannot corrupt this figure.
  const quarterTurned = ((Math.round(quickDeg / 90) % 2) + 2) % 2 === 1;
  const viewCssW = quarterTurned ? ch : cw;
  const viewCssH = quarterTurned ? cw : ch;
  const viewW = quarterTurned ? panelH : panelW;
  const viewH = quarterTurned ? panelW : panelH;
  const view: PanelPoint = {
    x: clampRound(((dx + viewCssW / 2) * viewW) / viewCssW, viewW),
    y: clampRound(((dy + viewCssH / 2) * viewH) / viewCssH, viewH),
  };

  return { panel, view, viewW, viewH };
}
