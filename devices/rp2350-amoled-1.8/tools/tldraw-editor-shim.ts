/**
 * Stands in for `@tldraw/editor` so tldraw's own freehand sources under
 * tools/tldraw-freehand/ can run here byte-identical to upstream.
 *
 * Those files only need two things from the editor package, and both are used
 * purely as types by the code paths we call (ingest + computeRadii), so nothing
 * of tldraw's actual runtime is being reimplemented here.
 */

export interface VecLike {
  x: number;
  y: number;
  z?: number;
}

export class Vec implements VecLike {
  constructor(
    public x: number = 0,
    public y: number = 0,
    public z: number = 1,
  ) {}
}
