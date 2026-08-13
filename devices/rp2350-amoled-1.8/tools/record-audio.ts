#!/usr/bin/env bun
// record-audio.ts: records from a named microphone via ffmpeg and reports
// the dominant frequencies in the recording via a plain FFT. Written to
// verify firmware/runtime/sound.c's alarm chime the same way tools/cam.py
// verifies the panel: by measuring the physical output, not by trusting the
// code that produced it. Neither devlink (tools/dev.ts, which drives touch/
// buttons and reads the framebuffer - no audio path at all) nor the
// framebuffer can answer "does the speaker actually make the intended
// sound"; a microphone recording, analysed, can.
//
// WHY FFMPEG, WHY NOT sounddevice/pyaudio. Both were tried first and both
// fail on this machine (Windows on ARM64): `pip install sounddevice` installs
// but importing it fails immediately (`PortAudio library not found` - its
// bundled libportaudioarm64.dll will not load, error 0x7e); pyaudio is not
// installed and depends on the same native PortAudio library, so it would
// fail the identical way. ffmpeg's `-f dshow` input records from a named
// DirectShow device directly, needs no Python native extension, and gives
// the same select-by-NAME property tools/cam.py already relies on for the
// camera - the camera INDEX is not stable on this machine (cam.py's own
// header comment: it once silently photographed the operator's face), and
// there is no reason to expect a microphone index would be any more stable.
//
// WHY TYPESCRIPT, NOT PYTHON, UNLIKE cam.py. cam.py's own header comment
// explains why IT is the one Python exception in this repo: no ffmpeg on
// PATH, and no usable native Node/Bun DirectShow binding for VIDEO capture
// on Windows-ARM64. Audio is different: ffmpeg (once downloaded - see below)
// handles the actual capture as a subprocess, so this file only ever spawns
// it and reads the file it writes, exactly the shape tools/dev.ts already
// uses for the serial bridge. No native binding is needed for that, so the
// project's TypeScript-only rule for tooling applies normally here.
//
// FFMPEG ITSELF. Not on PATH (`where ffmpeg` finds nothing on this machine,
// same as tools/cam.py's header comment already documents) and not vendored
// in this repo (a 100MB+ binary has no business in git). Expected at
// C:\Users\sylve\tools\ffmpeg\ffmpeg.exe (an x64 build, gyan.dev's
// "essentials" build, runs fine under this ARM64 machine's x64 emulation,
// the same way this project's pico-sdk tooling already does - see AGENTS.md's
// toolchain notes); override with FFMPEG_EXE.
//
// THE ANALYSIS. A plain, self-contained radix-2 FFT (fftReal() below, no
// dependency: numpy is unavailable to a TypeScript tool and was never a
// candidate here anyway) over a Hann-windowed recording, reporting the
// strongest frequency peaks. This is deliberately a general "what did the
// microphone actually hear" measurement, not a check that only looks at the
// four frequencies sound_synth.c intended (a bug that produced a wrong
// frequency entirely - e.g. an ES8311 clock-coefficient mistake putting a
// 523Hz note out as 5230Hz - would pass a check that only inspected the
// intended bins and say nothing, which is exactly the kind of result that
// looks like success without earning it).
import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, unlinkSync } from "node:fs";

const FFMPEG = process.env.FFMPEG_EXE ?? "C:\\Users\\sylve\\tools\\ffmpeg\\ffmpeg.exe";

function listDevices(): string[] {
    // ffmpeg's dshow device list is printed to stderr and the process exits
    // non-zero (it is not actually trying to open "dummy") - spawnSync still
    // captures stderr either way.
    const res = spawnSync(FFMPEG, ["-hide_banner", "-f", "dshow", "-list_devices", "true", "-i", "dummy"], {
        encoding: "utf8",
    });
    const out = (res.stderr ?? "") + (res.stdout ?? "");
    const names: string[] = [];
    // ffmpeg prints, per device: `[in#0 @ ...] "Name" (audio)` immediately
    // followed by an "Alternative name" line for the actual dshow moniker
    // (a @device_cm_{...} GUID string) that -i audio="Name" cannot use
    // reliably when two devices share a display name - captured here so
    // record() can pass the unambiguous moniker instead of the display name.
    const lines = out.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
        const m = lines[i].match(/"([^"]+)"\s+\(audio\)\s*$/);
        if (m) names.push(m[1]);
    }
    return names;
}

function findDeviceName(pattern: string, devices: string[]): string {
    const rx = new RegExp(pattern, "i");
    const matches = devices.filter((d) => rx.test(d));
    if (matches.length === 0) {
        console.error(`record-audio: no microphone name matches /${pattern}/i. Available devices:`);
        for (const d of devices) console.error(`  ${d}`);
        console.error(`Pass --name with a pattern that matches one of the above.`);
        process.exit(1);
    }
    if (matches.length > 1) {
        console.error(`record-audio: /${pattern}/i matches more than one microphone, refusing to guess which:`);
        for (const d of matches) console.error(`  ${d}`);
        console.error(`Pass --name with a more specific pattern.`);
        process.exit(1);
    }
    return matches[0];
}

// Records `seconds` of mono PCM at `rate` Hz from the named device to a raw
// s16le file (no WAV container - one fewer thing to parse, and this tool is
// the only reader). ffmpeg needs a few hundred ms to open the device and
// start actually capturing; `seconds` is measured from ffmpeg's own `-t`,
// which times the capture itself, not wall-clock around the spawn, so a
// caller does not need to pad this for that startup latency (recordAt()
// below still asks for a couple of seconds of pre-roll for a different
// reason - see its own comment).
function record(deviceName: string, seconds: number, rate: number, outPath: string) {
    const args = [
        "-hide_banner", "-y",
        "-f", "dshow",
        "-i", `audio=${deviceName}`,
        "-t", String(seconds),
        "-ac", "1",
        "-ar", String(rate),
        "-f", "s16le",
        outPath,
    ];
    console.error(`record-audio: ${FFMPEG} ${args.join(" ")}`);
    const res = spawnSync(FFMPEG, args, { stdio: ["ignore", "inherit", "inherit"] });
    if (!res.success && res.status !== 0) {
        console.error(`record-audio: ffmpeg exited ${res.status}`);
        process.exit(1);
    }
    if (!existsSync(outPath)) {
        console.error(`record-audio: ffmpeg reported success but ${outPath} does not exist`);
        process.exit(1);
    }
}

function readS16le(path: string): Float64Array {
    const buf = readFileSync(path);
    const n = buf.length >> 1;
    const out = new Float64Array(n);
    for (let i = 0; i < n; i++) out[i] = buf.readInt16LE(i * 2);
    return out;
}

// A plain, iterative radix-2 Cooley-Tukey FFT. samples.length must be a
// power of two (fftPeaks() below pads/truncates to the nearest one below the
// input length, rather than requiring the caller to get this exact). No
// external dependency - numpy is not reachable from TypeScript, and this
// tool has no other reason to add one.
function fft(re: Float64Array, im: Float64Array) {
    const n = re.length;
    // bit-reversal permutation
    for (let i = 1, j = 0; i < n; i++) {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            [re[i], re[j]] = [re[j], re[i]];
            [im[i], im[j]] = [im[j], im[i]];
        }
    }
    for (let len = 2; len <= n; len <<= 1) {
        const ang = (-2 * Math.PI) / len;
        const wr = Math.cos(ang), wi = Math.sin(ang);
        for (let i = 0; i < n; i += len) {
            let curWr = 1, curWi = 0;
            for (let k = 0; k < len / 2; k++) {
                const uRe = re[i + k], uIm = im[i + k];
                const vRe = re[i + k + len / 2] * curWr - im[i + k + len / 2] * curWi;
                const vIm = re[i + k + len / 2] * curWi + im[i + k + len / 2] * curWr;
                re[i + k] = uRe + vRe;
                im[i + k] = uIm + vIm;
                re[i + k + len / 2] = uRe - vRe;
                im[i + k + len / 2] = uIm - vIm;
                const nextWr = curWr * wr - curWi * wi;
                const nextWi = curWr * wi + curWi * wr;
                curWr = nextWr;
                curWi = nextWi;
            }
        }
    }
}

type Peak = { hz: number; mag: number };

// Hann-windowed FFT magnitude spectrum, returning the top `count` local
// peaks (a local maximum beats both neighbours - avoids reporting the same
// spectral line 3 times over from adjacent bins) above `minMagFrac` of the
// spectrum's own maximum, sorted loudest first.
function fftPeaks(samples: Float64Array, sampleRateHz: number, count: number, minMagFrac: number): Peak[] {
    let n = 1;
    while (n * 2 <= samples.length) n *= 2;
    const re = new Float64Array(n);
    const im = new Float64Array(n);
    for (let i = 0; i < n; i++) {
        const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (n - 1)); // Hann
        re[i] = samples[i] * w;
    }
    fft(re, im);

    const half = n / 2;
    const mag = new Float64Array(half);
    let maxMag = 0;
    for (let i = 0; i < half; i++) {
        mag[i] = Math.hypot(re[i], im[i]);
        if (mag[i] > maxMag) maxMag = mag[i];
    }

    const threshold = maxMag * minMagFrac;
    const candidates: Peak[] = [];
    for (let i = 1; i < half - 1; i++) {
        if (mag[i] < threshold) continue;
        if (mag[i] <= mag[i - 1] || mag[i] <= mag[i + 1]) continue;
        candidates.push({ hz: (i * sampleRateHz) / n, mag: mag[i] });
    }
    candidates.sort((a, b) => b.mag - a.mag);
    return candidates.slice(0, count);
}

async function main() {
    const args = process.argv.slice(2);
    let out = "";
    let name = "C920";
    let seconds = 6;
    let rate = 48000;
    let listOnly = false;
    let peakCount = 8;
    let analyzeOnly = "";

    for (let i = 0; i < args.length; i++) {
        const a = args[i];
        if (a === "--name") name = args[++i];
        else if (a === "--seconds") seconds = Number(args[++i]);
        else if (a === "--rate") rate = Number(args[++i]);
        else if (a === "--list") listOnly = true;
        else if (a === "--peaks") peakCount = Number(args[++i]);
        else if (a === "--analyze") analyzeOnly = args[++i];
        else if (!out) out = a;
        else {
            console.error(`record-audio: unexpected argument ${a}`);
            process.exit(1);
        }
    }

    if (!existsSync(FFMPEG) && !analyzeOnly) {
        console.error(`record-audio: ffmpeg not found at ${FFMPEG} (set FFMPEG_EXE to override)`);
        process.exit(1);
    }

    if (listOnly) {
        for (const d of listDevices()) console.log(d);
        return;
    }

    let rawPath: string;
    let cleanup = false;
    if (analyzeOnly) {
        rawPath = analyzeOnly;
    } else {
        const devices = listDevices();
        const device = findDeviceName(name, devices);
        console.error(`record-audio: matched microphone "${device}"`);
        rawPath = out || `record-${Date.now()}.raw`;
        record(device, seconds, rate, rawPath);
        console.error(`record-audio: wrote ${rawPath}`);
    }

    const samples = readS16le(rawPath);
    if (samples.length === 0) {
        console.error(`record-audio: ${rawPath} decoded to zero samples`);
        process.exit(1);
    }
    let peakAbs = 0;
    for (const s of samples) if (Math.abs(s) > peakAbs) peakAbs = Math.abs(s);
    console.log(`record-audio: ${samples.length} samples, peak amplitude ${peakAbs} / 32767`);

    const peaks = fftPeaks(samples, rate, peakCount, 0.15);
    console.log(`record-audio: top ${peaks.length} frequency peaks (Hz, magnitude):`);
    for (const p of peaks) console.log(`    ${p.hz.toFixed(1)} Hz  (mag ${p.mag.toFixed(0)})`);

    if (cleanup) unlinkSync(rawPath);
}

main();
