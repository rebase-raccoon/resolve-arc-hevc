#!/usr/bin/env python3
"""Decode with Resolve's real ABI; compare its pixels/metadata to the adapter.

The software-test backend verifies ABI conversion only. --hardware requires
actual VA-API frames and must be run where /dev/dri is available.
"""
import argparse
import ctypes as C
import faulthandler
import hashlib
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parent
DEFAULT = Path(__file__).resolve().parent/'controls'/'validation-4k30-420-10bit-hlg.mov'


def worker(path, count, cycles, timings=None):
    p = C.c_void_p
    lib = C.CDLL(None)

    def api(name, args, result=C.c_int):
        f = getattr(lib, name)
        f.argtypes, f.restype = args, result
        return f

    class Rat(C.Structure):
        _fields_ = [('num', C.c_int), ('den', C.c_int)]

    class Context(C.Structure):
        _fields_ = [('avclass', p), ('log_offset', C.c_int), ('codec_type', C.c_int),
                    ('codec', p), ('codec_id', C.c_int), ('codec_tag', C.c_uint),
                    ('private', p), ('internal', p), ('opaque', p), ('bitrate', C.c_int64),
                    ('tolerance', C.c_int), ('quality', C.c_int), ('compression', C.c_int),
                    ('flags', C.c_int), ('flags2', C.c_int), ('extradata', p),
                    ('extradata_size', C.c_int), ('timebase', Rat), ('ticks', C.c_int),
                    ('delay', C.c_int), ('width', C.c_int), ('height', C.c_int),
                    ('coded_width', C.c_int), ('coded_height', C.c_int),
                    ('gop_size', C.c_int), ('format', C.c_int)]

    class Format(C.Structure):
        _fields_ = [('avclass', p), ('input', p), ('output', p), ('private', p),
                    ('io', p), ('flags', C.c_int), ('nstreams', C.c_uint), ('streams', C.POINTER(p))]

    class Stream(C.Structure):
        _fields_ = [('avclass', p), ('index', C.c_int), ('id', C.c_int),
                    ('params', p), ('private', p), ('timebase', Rat)]

    class Packet(C.Structure):
        _fields_ = [('buf', p), ('pts', C.c_int64), ('dts', C.c_int64), ('data', p),
                    ('size', C.c_int), ('stream_index', C.c_int)]

    class Frame(C.Structure):
        _fields_ = [('data', p*8), ('linesize', C.c_int*8), ('extended_data', p),
                    ('width', C.c_int), ('height', C.c_int), ('samples', C.c_int),
                    ('format', C.c_int), ('key', C.c_int), ('picture', C.c_int),
                    ('sar', Rat), ('pts', C.c_int64), ('dts', C.c_int64), ('tb', Rat),
                    ('coded', C.c_int), ('display', C.c_int), ('quality', C.c_int),
                    ('opaque', p), ('repeat', C.c_int), ('interlaced', C.c_int),
                    ('top', C.c_int), ('palette', C.c_int), ('reordered', C.c_int64),
                    ('rate', C.c_int), ('layout', C.c_uint64), ('buf', p*8),
                    ('extended_buf', p), ('n_extended', C.c_int), ('side_data', p),
                    ('n_side', C.c_int), ('flags', C.c_int), ('range', C.c_int),
                    ('primaries', C.c_int), ('transfer', C.c_int), ('space', C.c_int),
                    ('chroma', C.c_int), ('best_pts', C.c_int64), ('pos', C.c_int64),
                    ('duration', C.c_int64)]

    open_input = api('avformat_open_input', [C.POINTER(p), C.c_char_p, p, p])
    info = api('avformat_find_stream_info', [p, p])
    best = api('av_find_best_stream', [p, C.c_int, C.c_int, C.c_int, C.POINTER(p), C.c_int])
    alloc = api('avcodec_alloc_context3', [p], p)
    params = api('avcodec_parameters_to_context', [p, p])
    opt = api('av_opt_set', [p, C.c_char_p, C.c_char_p, C.c_int])
    opt_q = api('av_opt_set_q', [p, C.c_char_p, Rat, C.c_int])
    open_codec = api('avcodec_open2', [p, p, p])
    packet_alloc = api('av_packet_alloc', [], p)
    frame_alloc = api('av_frame_alloc', [], p)
    read = api('av_read_frame', [p, p])
    send = api('avcodec_send_packet', [p, p])
    recv = api('avcodec_receive_frame', [p, p])
    unref = api('av_packet_unref', [p], None)
    frame_unref = api('av_frame_unref', [p], None)
    flush = api('avcodec_flush_buffers', [p], None)
    seek = api('av_seek_frame', [p, C.c_int, C.c_int64, C.c_int])
    close = api('avcodec_free_context', [C.POINTER(p)], None)
    frame_free = api('av_frame_free', [C.POINTER(p)], None)
    packet_free = api('av_packet_free', [C.POINTER(p)], None)
    format_free = api('avformat_close_input', [C.POINTER(p)], None)
    image_size = api('av_image_get_buffer_size', [C.c_int, C.c_int, C.c_int, C.c_int])
    image_copy = api('av_image_copy_to_buffer', [p, C.c_int, p, p, C.c_int, C.c_int, C.c_int, C.c_int])
    pix_name = api('av_get_pix_fmt_name', [C.c_int], C.c_char_p)
    side_name = api('av_frame_side_data_name', [C.c_int], C.c_char_p)

    class Side(C.Structure):
        _fields_ = [('type', C.c_int), ('data', p), ('size', C.c_size_t)]

    fmt, ctx, packet, frame, test_frame = p(), p(), p(), p(), p()
    transfer_test = api('arc_transfer_test', [p, p]) if os.environ.get('ARC_TEST_TRANSFER') else None
    results = []
    no_hash=os.environ.get('ARC_BENCH_NO_HASH')=='1'
    try:
        assert open_input(C.byref(fmt), os.fsencode(path), None, None) >= 0
        assert info(fmt, None) >= 0
        codec = p()
        stream_index = best(fmt, 0, -1, -1, C.byref(codec), 0)
        assert stream_index >= 0
        fctx = C.cast(fmt, C.POINTER(Format)).contents
        stream = C.cast(fctx.streams[stream_index], C.POINTER(Stream)).contents
        ctx = p(alloc(codec))
        assert params(ctx, stream.params) >= 0
        if os.environ.get('ARC_DEFER_CONTEXT'):
            header = C.cast(ctx, C.POINTER(Context)).contents
            header.width = header.height = header.coded_width = header.coded_height = 0
            header.format = -1
        assert opt(ctx, b'threads', b'1', 0) >= 0
        assert opt_q(ctx, b'pkt_timebase', stream.timebase, 0) >= 0
        opened = open_codec(ctx, codec, None)
        assert opened >= 0, f'decoder open failed: {opened}'
        packet, frame = p(packet_alloc()), p(frame_alloc())
        if transfer_test:
            test_frame = p(frame_alloc())

        def receive(records):
            while True:
                ret = recv(ctx, frame)
                if ret in (-11, -541478725):
                    return ret
                assert ret >= 0, f'receive failed: {ret}'
                visible = frame
                if transfer_test:
                    assert transfer_test(frame, test_frame) == 0
                    visible = test_frame
                fr = C.cast(visible, C.POINTER(Frame)).contents
                header = C.cast(ctx, C.POINTER(Context)).contents
                assert [header.width, header.height, header.format] == [fr.width, fr.height, fr.format], \
                    f'Decoder context is stale: {[header.width, header.height, header.format]} vs frame {[fr.width, fr.height, fr.format]}'
                digest=None
                if not no_hash:
                    n = image_size(fr.format, fr.width, fr.height, 1)
                    assert n > 0
                    buf = C.create_string_buffer(n)
                    assert image_copy(buf, n, fr.data, fr.linesize, fr.format, fr.width, fr.height, 1) == n
                    digest=hashlib.sha256(buf.raw).hexdigest()
                side = []
                if fr.n_side:
                    array = C.cast(fr.side_data, C.POINTER(p))
                    for i in range(fr.n_side):
                        sd = C.cast(array[i], C.POINTER(Side)).contents
                        side.append([side_name(sd.type).decode(), hashlib.sha256(C.string_at(sd.data, sd.size)).hexdigest()])
                records.append({'hash': digest,
                                'size': [fr.width, fr.height], 'format': pix_name(fr.format).decode(),
                                'pts': fr.pts, 'dts': fr.dts, 'best_pts': fr.best_pts,
                                'duration': fr.duration, 'sar': [fr.sar.num, fr.sar.den],
                                'color': [fr.range, fr.primaries, fr.transfer, fr.space, fr.chroma],
                                'key': fr.key, 'picture': fr.picture, 'side_data': sorted(side)})
                frame_unref(frame)
                if test_frame:
                    frame_unref(test_frame)

        for cycle in range(cycles):
            if cycle:
                assert seek(fmt, stream_index, 0, 1) >= 0
                flush(ctx)
            records = []
            packets = 0
            started=time.perf_counter_ns()
            while packets < count and read(fmt, packet) >= 0:
                pkt = C.cast(packet, C.POINTER(Packet)).contents
                if pkt.stream_index == stream_index:
                    ret = send(ctx, packet)
                    if ret == -11:
                        receive(records)
                        ret = send(ctx, packet)
                    assert ret >= 0, f'send failed: {ret}'
                    packets += 1
                    receive(records)
                unref(packet)
            assert send(ctx, None) >= 0
            assert receive(records) == -541478725
            assert records
            if timings is not None:
                timings.append({'frames':len(records),'elapsed_ns':time.perf_counter_ns()-started})
            results.append(records)
    finally:
        if ctx: close(C.byref(ctx))
        if packet: packet_free(C.byref(packet))
        if frame: frame_free(C.byref(frame))
        if test_frame: frame_free(C.byref(test_frame))
        if fmt: format_free(C.byref(fmt))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip', nargs='?', type=Path, default=DEFAULT)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--hardware', action='store_true')
    mode.add_argument('--planes', action='store_true', help='Test P010/NV12 packing and unpacking without a GPU')
    parser.add_argument('--defer-context', action='store_true',
                        help='Discover frame dimensions/pixel format from the bitstream, as Resolve can do')
    parser.add_argument('--allow-encoder-sei-differences', action='store_true',
                        help='Ignore repetition of encoder-identification SEI in synthetic x265 fixtures')
    parser.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--packets', type=int, default=32)
    parser.add_argument('--cycles', type=int, default=2)
    parser.add_argument('--threads', type=int, default=1, help='Decode with multiple independent contexts concurrently')
    args = parser.parse_args()
    if args.worker:
        faulthandler.enable()
        if args.threads>1:
            with ThreadPoolExecutor(max_workers=args.threads) as pool:
                runs=list(pool.map(lambda _: worker(args.clip,args.packets,args.cycles),range(args.threads)))
            result=[cycle for run in runs for cycle in run]
        else:
            result=worker(args.clip,args.packets,args.cycles)
        print(json.dumps(result))
        return
    env = os.environ.copy()
    env.pop('LD_PRELOAD', None)
    env.pop('LD_LIBRARY_PATH', None)
    env.pop('RESOLVE_ARC_MODE', None)
    env.pop('ARC_TEST_TRANSFER', None)
    env.pop('ARC_DEFER_CONTEXT', None)
    env.pop('ARC_BENCH_NO_HASH', None)
    if args.defer_context:
        env['ARC_DEFER_CONTEXT'] = '1'
    bundled = '/opt/resolve/libs'
    preload = ':'.join(f'{bundled}/{name}' for name in ['libavformat.so.60', 'libavcodec.so.60', 'libavutil.so.58'])
    env['LD_PRELOAD'] = preload
    command = [sys.executable, str(Path(__file__).resolve()), str(args.clip), '--worker',
               '--packets', str(args.packets), '--cycles', str(args.cycles), '--threads',str(args.threads)]
    baseline = subprocess.run(command, env=env, capture_output=True, text=True)
    if baseline.returncode:
        raise RuntimeError(f'Baseline failed ({baseline.returncode}): {baseline.stderr}')
    ROOT.joinpath('logs').mkdir(exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix='test-', suffix='.log', dir=ROOT/'logs', delete=False) as f:
        log = Path(f.name)
    shim = 'libtransfer_test.so' if args.planes else 'libresolve_arc.so'
    env['LD_PRELOAD'] = f'{ROOT}/{shim}:{preload}'
    if args.planes:
        env['ARC_TEST_TRANSFER'] = '1'
    else:
        env['RESOLVE_ARC_MODE'] = 'vaapi' if args.hardware else 'software-test'
    env['RESOLVE_ARC_LOG'] = str(log)
    bridged = subprocess.run(command, env=env, capture_output=True, text=True)
    print(f'Adapter evidence: {log}')
    diagnostics = log.with_suffix('.stderr.txt')
    if bridged.stderr:
        diagnostics.write_text(bridged.stderr)
    if bridged.returncode:
        print(log.read_text())
        reason = (signal.Signals(-bridged.returncode).name if bridged.returncode < 0
                  else f'exit status {bridged.returncode}')
        detail = str(diagnostics) if bridged.stderr else 'No stderr was emitted; see the adapter evidence log.'
        raise RuntimeError(f'Adapter failed ({reason}). Diagnostics: {detail}')
    a, b = json.loads(baseline.stdout), json.loads(bridged.stdout)
    if args.allow_encoder_sei_differences:
        def normalize(cycles):
            payloads = set()
            for records in cycles:
                for record in records:
                    keep = []
                    for name, digest in record['side_data']:
                        if name == 'H.26[45] User Data Unregistered SEI message':
                            payloads.add(digest)
                        else:
                            keep.append([name, digest])
                    record['side_data'] = keep
            return payloads
        assert normalize(a) == normalize(b), 'Unregistered SEI payloads changed'
    if a != b:
        for cycle, (aa, bb) in enumerate(zip(a, b)):
            print(f'Cycle {cycle}: baseline {len(aa)} frames; adapter {len(bb)} frames')
            for i, (x, y) in enumerate(zip(aa, bb)):
                if x != y:
                    print(f'First differing frame {i}:')
                    print(json.dumps({'baseline': x, 'adapter': y}, indent=2))
                    break
        raise AssertionError('Pixels or metadata differ')
    evidence = log.read_text()
    assert 'event=backend namespace=' in evidence
    if not args.planes:
        assert 'event=open ' in evidence
    assert 'failed=1' not in evidence and 'event=receive_failed' not in evidence
    if not args.planes:
        assert f'flushes={args.cycles-1}' in evidence
    if args.hardware:
        assert 'event=hardware_format ' in evidence
        frames = [line for line in evidence.splitlines() if line.startswith('event=frame ')]
        assert frames and all('hardware_frames=0 ' not in line for line in frames)
    print(f'PASS: {len(a)} decode/seek cycles, {sum(map(len,a))} frames; pixels, timestamps, color and side data match Resolve FFmpeg.')
    if not args.hardware:
        print('This validates the ABI adapter using software decoding. Arc decoding is NOT proven by this test.')
    if args.allow_encoder_sei_differences:
        print('Unregistered encoder SEI payloads match; their per-frame repetition was excluded from comparison.')


if __name__ == '__main__':
    main()
