#!/usr/bin/env python3
"""ArtInChip D12x UART 烧录上位机（Linux，无需 AiBurn/USB）。

协议来源（已实测验证）：
- 《D12x 用户指南》7.4.2 在线升级模式
- luban-lite application/baremetal/bootloader/lib/aicupg/ 设备端实现
- upgcmd 反汇编（CBW 的 bCBWCBLength=1、头/数据/响应分事务）

传输层（UART 115200 8N1）：
- 从机持续发 'A'(0x41) 表示就绪，主机回 ACK(0x06) 建立连接
- 数据用 SOH/STX 帧封装，块号从 1 开始递增，CRC16-CCITT，逐帧 ACK/NAK
- 设备不主动发数据：主机 DC2(0x12) 切换到主机接收，收完 DC1(0x11) 切回
- 注意：设备块号计数器跨连接保持，仅断电复位清零；重复/乱序帧会被回 CAN(0x18)

传输层之上是 USB 风格 CBW/CSW 事务（bCBWCBLength=1）：
- 写事务：CBW(WRITE)+数据帧 → DC2 → 收 CSW → DC1
- 读事务：CBW(READ) → DC2 → 收 resp 数据帧 + CSW → DC1

用法：
  aic_uart_upg.py info               # 连接并读取芯片信息
  aic_uart_upg.py burn <image.img>   # 整包烧录（约 8 分钟 @115200）
  aic_uart_upg.py reset              # 发送 shell "reset" 复位
"""

import argparse
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("需要 pyserial：pip3 install pyserial")

# ---- 帧层常量 ----
SOH, STX = 0x01, 0x02
ACK, NAK = 0x06, 0x15
DC1_SEND, DC2_RECV = 0x11, 0x12
CAN, SIG_A, SIG_C = 0x18, 0x41, 0x43
LNG_FRM_DLEN, SHT_FRM_DLEN = 1024, 176

# ---- 应用层常量 ----
UPGC, UPGR = 0x43475055, 0x52475055          # "UPGC" / "UPGR"
USBC, USBS = 0x43425355, 0x53425355          # CBW / CSW 签名
PROTO_TYPE = PROTO_VER = 0x01
TRANS_WRITE, TRANS_READ = 0x01, 0x02

CMD_GET_HWINFO = 0x00
CMD_RUN_SHELL_STR = 0x05
CMD_SET_FWC_META = 0x10
CMD_GET_BLOCK_SIZE = 0x11
CMD_SEND_FWC_DATA = 0x12
CMD_GET_FWC_CRC = 0x13
CMD_GET_FWC_BURN_RESULT = 0x14
CMD_GET_FWC_RUN_RESULT = 0x15

HWINFO_LEN = 108
FWC_META_LEN = 512


def crc16(data: bytes, cksum: int = 0) -> int:
    """CRC16-CCITT (poly 0x1021, init 0)，与 luban-lite 一致。"""
    tab = _CRC16_TAB
    for b in data:
        cksum = tab[((cksum >> 8) ^ b) & 0xFF] ^ ((cksum << 8) & 0xFFFF)
    return cksum

_CRC16_TAB = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) & 0xFFFF if _c & 0x8000 else (_c << 1) & 0xFFFF
    _CRC16_TAB.append(_c)


class UpgError(Exception):
    pass


class AicUpg:
    def __init__(self, port: str, baud: int = 115200, log=print):
        self.s = serial.Serial(port, baud, timeout=0.3)
        # DTR/RTS 默认会被拉高，若接到板上自动烧录电路会干扰芯片，显式拉低
        self.s.dtr = False
        self.s.rts = False
        self.log = log
        self.blk = 1      # 主机->设备 帧号（从 1 开始）
        self.tag = 0      # CBW tag
        self.connected = False

    # ---------- 连接 ----------
    def connect(self, timeout=10):
        """建立连接：BROM 阶段广播 'A' 回 ACK；updater 阶段不广播，
        用 SIG_C 探测。都不行时乐观放行——设备端按字节流重组帧，
        下一条命令的 31 字节 CBW 帧会自动对齐其挂起的 CBW 读。"""
        self.s.reset_input_buffer()
        t0 = time.time()
        buf = b''
        while time.time() - t0 < timeout:
            buf += self.s.read(64)
            if b'A' in buf:
                self.s.write(bytes([ACK]))
                self.s.flush()
                self.blk = self.blk or 1
                self.tag = 0
                self.connected = True
                return True
            if time.time() - t0 > 1.5:
                self.s.write(b'C')
                self.s.flush()
                r = self.s.read(1)
                if r == b'\x06':
                    self.blk = self.blk or 1
                    self.tag = 0
                    self.connected = True
                    return True
                time.sleep(0.3)
        # 乐观放行（静默阶段）
        self.blk = self.blk or 1
        self.tag = 0
        self.connected = True
        self.log('  (无连接信号，乐观假设设备在静默升级阶段)')
        return True

    def resync_blk(self, max_try=64):
        """设备块号计数器跨连接保持。探测其期望的下一块号。"""
        probe = self._cmd_header(CMD_GET_HWINFO, 0)
        for guess in range(1, max_try + 1):
            if not self.connect(timeout=5):
                break
            if self._send_frame_raw(guess, probe):
                # ACK：guess 即设备期望的块号，该命令已被设备接收
                self.blk = (guess + 1) & 0xFF or 1
                return True
        return False

    def _send_frame_raw(self, blk: int, data: bytes):
        if len(data) == LNG_FRM_DLEN:
            head = bytes([STX, blk, (255 - blk) & 0xFF])
        else:
            head = bytes([SOH, blk, (255 - blk) & 0xFF, len(data)])
        c = crc16(data)
        frame = head + data + bytes([c >> 8, c & 0xFF])
        deadline = time.time() + 2
        while time.time() < deadline:
            self.s.write(frame)
            self.s.flush()
            r = self.s.read(1)
            if r == b'\x06':
                return True
            if r in (b'\x15', b'\x18', b''):
                return False
        return False

    def ensure_connected(self, timeout=8):
        if self.connected:
            return True
        if self.connect(timeout):
            return True
        raise UpgError('设备未进入烧写模式（未收到连接信号 A）')

    # ---------- 帧层：发送 ----------
    def _send_frame(self, data: bytes):
        assert 1 <= len(data) <= LNG_FRM_DLEN
        if len(data) == LNG_FRM_DLEN:
            head = bytes([STX, self.blk, (255 - self.blk) & 0xFF])
        else:
            head = bytes([SOH, self.blk, (255 - self.blk) & 0xFF, len(data)])
        c = crc16(data)
        frame = head + data + bytes([c >> 8, c & 0xFF])
        deadline = time.time() + 3
        while time.time() < deadline:
            self.s.write(frame)
            self.s.flush()
            r = self.s.read(1)
            if os.environ.get('AICUPG_DEBUG'):
                print(f'    [帧 blk={self.blk} len={len(data)}] -> {r.hex() or "无"}', flush=True)
            if r == b'\x06':
                self.blk = (self.blk + 1) & 0xFF or 1
                return
            if r == b'\x15':
                continue
            if r == b'\x18':
                raise UpgError('设备回 CAN（块号失步，需断电复位板子）')
            if r == b'':
                # 无应答：设备可能掉线重连中，尝试重新握手
                self.connected = False
                if self.connect(timeout=2):
                    continue
                continue
            # 其他字节丢弃重试
        raise UpgError('帧发送无 ACK')

    def _send_data(self, payload: bytes):
        i, n = 0, len(payload)
        while i < n:
            rest = n - i
            k = LNG_FRM_DLEN if rest > LNG_FRM_DLEN else (SHT_FRM_DLEN if rest > SHT_FRM_DLEN else rest)
            self._send_frame(payload[i:i + k])
            i += k

    # ---------- 帧层：接收 ----------
    def _read_frame(self, timeout=3.0) -> bytes:
        deadline = time.time() + timeout
        while time.time() < deadline:
            b = self.s.read(1)
            if not b:
                continue
            if b[0] == SIG_A:
                self.connected = False
                raise UpgError('设备掉线（重新进入等待连接）')
            if b[0] not in (SOH, STX):
                if os.environ.get('AICUPG_DEBUG'):
                    print(f'    [收帧跳过字节] 0x{b[0]:02x}', flush=True)
                continue
            if b[0] == SOH:
                hdr = self.s.read(3)
                if len(hdr) < 3:
                    continue
                dlen = hdr[2]
            else:
                self.s.read(2)
                dlen = LNG_FRM_DLEN
            body = self.s.read(dlen + 2)
            if len(body) < dlen + 2:
                continue
            data, crc_rx = body[:dlen], (body[dlen] << 8) | body[dlen + 1]
            if crc16(data) == crc_rx:
                if os.environ.get('AICUPG_DEBUG'):
                    print(f'    [收到帧 {len(data)}B] {data[:24].hex()}', flush=True)
                self.s.write(bytes([ACK]))
                self.s.flush()
                return data
            if os.environ.get('AICUPG_DEBUG'):
                print(f'    [帧CRC错误 {len(data)}B] {data[:24].hex()} crc={crc_rx:04x} 期望={crc16(data):04x}', flush=True)
            self.s.write(bytes([NAK]))
            self.s.flush()
        raise UpgError('收帧超时')

    def _recv_data(self, total: int, timeout=10.0) -> bytes:
        out = b''
        deadline = time.time() + timeout
        while len(out) < total:
            left = deadline - time.time()
            if left <= 0:
                raise UpgError(f'收数据超时（{len(out)}/{total}）')
            out += self._read_frame(timeout=min(left, 5.0))
        return out[:total]

    # ---------- CBW/CSW 事务 ----------
    def _cbw(self, xfer: int, is_read: bool) -> bytes:
        tag = self.tag
        self.tag = (self.tag + 1) & 0xFFFFFFFF
        return struct.pack('<IIIBBBB15x', USBC, tag, xfer,
                           0x80 if is_read else 0x00, 0, 1,
                           TRANS_READ if is_read else TRANS_WRITE)

    def _csw(self, expect_tag: int, what: str):
        csw = self._recv_data(13, timeout=30)
        sig, tag, residue, status = struct.unpack('<IIIB', csw)
        if sig != USBS:
            raise UpgError(f'{what}: CSW 签名错误 0x{sig:08x}')
        if status != 0:
            raise UpgError(f'{what}: CSW status={status}')

    def trans_write(self, payload: bytes, timeout=30, no_csw=False):
        self.ensure_connected()
        self._send_data(self._cbw(len(payload), False))
        self._send_data(payload)
        if no_csw:
            # RUN 组件末帧：发完即返回，设备随时可能跳转新阶段
            self.tag = (self.tag + 1) & 0xFFFFFFFF
            return
        self._switch_dir(receive=True)
        try:
            self._csw(self.tag - 1, '写事务')
        finally:
            self._switch_dir(receive=False)

    def trans_read(self, length: int, timeout=60, no_csw=False) -> bytes:
        self.ensure_connected()
        self._send_data(self._cbw(length, True))
        self._switch_dir(receive=True)
        try:
            data = self._recv_data(length, timeout=timeout)
            if not no_csw:
                self._csw(self.tag - 1, '读事务')
            return data
        finally:
            self._switch_dir(receive=False)

    def _switch_dir(self, receive: bool):
        self.s.write(bytes([DC2_RECV if receive else DC1_SEND]))
        self.s.flush()
        time.sleep(0.02)
        r = self.s.read(1)
        if r != b'\x06':
            raise UpgError(f'DC{"2" if receive else "1"} 切换无 ACK')

    # ---------- 应用层命令 ----------
    def _cmd_header(self, cmd: int, dlen: int) -> bytes:
        ck = (UPGC + ((0 << 24) | (cmd << 16) | (PROTO_VER << 8) | PROTO_TYPE) + dlen) & 0xFFFFFFFF
        return struct.pack('<IBBBBII', UPGC, PROTO_TYPE, PROTO_VER, cmd, 0, dlen, ck)

    def cmd(self, cmd: int, payload: bytes = b'', resp_len: int | None = 0,
            timeout=60, split_data=True, no_csw=False):
        """按 upgcmd 模式执行一条命令：[头事务][数据事务] + [响应事务]。

        resp_len=None 不读响应；否则智能解析响应：
        BROM 部分命令响应带 16 字节 UPGR 头（GET_HWINFO/SET_FWC_META），
        部分直接回裸数据（GET_BLOCK_SIZE 只回 4 字节值）。
        返回 (status, data) 或 None。
        """
        hdr = self._cmd_header(cmd, len(payload))
        if payload and split_data:
            self.trans_write(hdr, no_csw=no_csw and resp_len is None)
            self.trans_write(payload, timeout=timeout, no_csw=no_csw)
        else:
            self.trans_write(hdr + payload, timeout=timeout, no_csw=no_csw)
        if resp_len is None:
            return None

        if no_csw:
            return None  # 设备即将跳转，读不到响应

        self.ensure_connected()
        self._send_data(self._cbw(16 + resp_len, True))
        self._switch_dir(receive=True)
        try:
            f1 = self._read_frame(timeout=timeout)
            if f1[:4] == b'UPGR':
                # 标准格式：[UPGR 头(16)][数据]
                magic, proto, ver, rcmd, status, dlen, ck = struct.unpack('<IBBBBII', f1[:16])
                data = f1[16:]
                if dlen > len(data):
                    data += self._recv_data(dlen - len(data), timeout=timeout)
                if not no_csw:
                    self._csw(self.tag - 1, '读事务')
                if status != 0:
                    raise UpgError(f'cmd 0x{cmd:02x}: 设备返回失败 status={status}')
                return status, data[:dlen]
            # 设备端 get_block_size/crc/burn/run 等命令的 memcpy(buf,&val,4) 会把
            # 4 字节值写到 buf 开头覆盖 magic：[值(4)][proto,ver,cmd,status][dlen][cksum]
            if len(f1) >= 16 and f1[4] == PROTO_TYPE and f1[5] == PROTO_VER:
                rcmd, status = f1[6], f1[7]
                dlen = struct.unpack_from('<I', f1, 8)[0]
                if rcmd != cmd:
                    raise UpgError(f'cmd 0x{cmd:02x}: 响应命令字不符 0x{rcmd:02x}')
                if status != 0:
                    raise UpgError(f'cmd 0x{cmd:02x}: 设备返回失败 status={status}')
                if not no_csw:
                    self._csw(self.tag - 1, '读事务')
                return status, f1[:dlen if dlen <= 4 else 4]
            # 纯数据帧（无头）——原样返回
            if not no_csw:
                self._csw(self.tag - 1, '读事务')
            return 0, f1
        finally:
            self._switch_dir(receive=False)

    def get_hwinfo(self) -> dict:
        status, data = self.cmd(CMD_GET_HWINFO, resp_len=HWINFO_LEN)
        return {'magic': data[:8], 'boot_stage': data[40], 'chipid': data[44:60].hex()}

    def set_fwc_meta(self, meta: bytes):
        self.cmd(CMD_SET_FWC_META, meta, resp_len=0, timeout=30)

    def get_block_size(self) -> int:
        status, data = self.cmd(CMD_GET_BLOCK_SIZE, resp_len=4)
        return struct.unpack('<I', data)[0]

    def send_fwc_data(self, chunk: bytes, final: bool, jump=False):
        # jump=True: RUN 组件末帧，设备跳转前无任何应答
        self.cmd(CMD_SEND_FWC_DATA, chunk,
                 resp_len=(None if jump else (0 if final else None)),
                 timeout=120, no_csw=jump)

    def get_fwc_crc(self) -> int:
        status, data = self.cmd(CMD_GET_FWC_CRC, resp_len=4)
        if len(data) >= 4:
            return struct.unpack('<I', data[-4:])[0]
        raise UpgError('GET_FWC_CRC 响应数据不足')

    def get_burn_result(self) -> int:
        status, data = self.cmd(CMD_GET_FWC_BURN_RESULT, resp_len=4)
        return struct.unpack('<i', data[-4:])[0]

    def get_run_result(self) -> int:
        status, data = self.cmd(CMD_GET_FWC_RUN_RESULT, resp_len=4)
        return struct.unpack('<i', data[-4:])[0]

    def run_shell(self, shell: str):
        s = shell.encode() + b'\x00'
        try:
            self.cmd(CMD_RUN_SHELL_STR, struct.pack('<I', len(s)) + s,
                     resp_len=4, timeout=15)
        except UpgError as e:
            self.log(f'  (shell 响应: {e}，忽略)')


# ---------- 镜像解析与烧录 ----------
def parse_image(path: str):
    img = open(path, 'rb').read()
    if img[:6] != b'AIC.FW':
        raise UpgError('不是 AIC 固件镜像')
    metas = []
    off = 0x800
    while img[off:off + 4] == b'META':
        m = img[off:off + 512]
        metas.append({
            'name': m[8:72].rstrip(b'\0').decode(),
            'offset': struct.unpack_from('<I', m, 136)[0],
            'size': struct.unpack_from('<I', m, 140)[0],
            'crc': struct.unpack_from('<I', m, 144)[0],
            'attr': m[152:216].rstrip(b'\0').decode(),
            'raw': m,
        })
        off += 512
    if not metas:
        raise UpgError('镜像中无 META 表')
    return metas, img


def do_component(upg: AicUpg, m: dict, blob: bytes):
    """烧录/运行单个组件（可安全重试：SET_FWC_META 会重置设备端组件状态）。"""
    is_run = 'run' in m['attr']
    t0 = time.time()

    upg.set_fwc_meta(m['raw'])
    bs = min(max(upg.get_block_size(), 4096), 1 << 20)
    sent = 0
    while sent < len(blob):
        chunk = blob[sent:sent + bs]
        final = sent + len(chunk) >= len(blob)
        if final and is_run:
            # RUN 组件末帧：头事务正常；数据事务发完即止（设备随时可能
            # 跳转新阶段，不再应答 DC2/CSW/DC1）
            upg.trans_write(upg._cmd_header(CMD_SEND_FWC_DATA, len(chunk)))
            upg.trans_write(chunk, timeout=120, no_csw=True)
            try:
                # BROM 未跳转：这是末帧响应；已跳转：该帧对齐新阶段挂起的 CBW 读
                upg.trans_read(16, timeout=3)
            except UpgError as e:
                upg.log(f"    (末帧收尾: {e})")
                upg.connected = False
                time.sleep(1.5)
                upg.connect(timeout=8)
        else:
            upg.send_fwc_data(chunk, final)
        sent += len(chunk)
        speed = sent / max(time.time() - t0, 0.001) / 1024
        print(f"\r    {sent * 100 // len(blob):3d}%  {speed:6.1f} KB/s", end='', flush=True)
    print()

    try:
        crc_got = upg.get_fwc_crc()
        if crc_got != m['crc']:
            raise UpgError(f"设备端 CRC 0x{crc_got:08x} != 0x{m['crc']:08x}")
    except UpgError as e:
        if not is_run:
            raise
        upg.log(f"    (RUN 组件 CRC 校验跳过: {e})")
    if not is_run:
        burn_res = upg.get_burn_result()
        if burn_res:
            raise UpgError(f"burn_result={burn_res}")
    else:
        # RUN 组件执行后设备可能已切换阶段，收尾命令容错
        try:
            run_res = upg.get_run_result()
            upg.log(f"    run_result={run_res}")
        except UpgError as e:
            upg.log(f"    (run_result 未获取: {e}，设备可能已跳转)")
        time.sleep(2.0)
    upg.log(f"    完成，耗时 {time.time() - t0:.1f}s")


def burn(upg: AicUpg, img_path: str):
    metas, img = parse_image(img_path)
    upg.log(f'镜像组件 {len(metas)} 个：')
    for m in metas:
        upg.log(f"  {m['name']:<24s} size={m['size']:>8d} attr={m['attr']}")

    for i, m in enumerate(metas, 1):
        blob = img[m['offset']:m['offset'] + m['size']]
        if zlib.crc32(blob) & 0xFFFFFFFF != m['crc']:
            raise UpgError(f"组件 {m['name']} 本地 CRC32 不符")
        is_run = 'run' in m['attr']
        upg.log(f"[{i}/{len(metas)}] {m['name']} ({m['size']} 字节, {'RUN' if is_run else 'BURN'})")

        last_err = None
        for attempt in range(1, 4):
            try:
                do_component(upg, m, blob)
                last_err = None
                break
            except UpgError as e:
                last_err = e
                upg.log(f"    第 {attempt} 次尝试失败: {e}")
                upg.connected = False
                time.sleep(1)
                if not upg.connect(timeout=5) and not upg.resync_blk():
                    upg.log('    重连失败，继续重试...')
                time.sleep(0.5)
        if last_err is not None:
            raise UpgError(f"组件 {m['name']} 三次尝试均失败: {last_err}")

    upg.log('全部组件烧录完毕，复位设备...')
    upg.run_shell('reset')


def run_with_resync(upg, fn, *a, **kw):
    """执行操作；若因块号失步（CAN）失败则自动重同步后重试一次。"""
    try:
        return fn(*a, **kw)
    except UpgError as e:
        if 'CAN' not in str(e):
            raise
        upg.log(f'块号失步（{e}），尝试自动重同步...')
        if not upg.resync_blk():
            raise
        upg.log(f'重同步成功，设备期望下一块 blk={upg.blk}')
        return fn(*a, **kw)


def main():
    ap = argparse.ArgumentParser(description='ArtInChip D12x UART 烧录工具')
    ap.add_argument('port', help='串口设备，如 /dev/ttyACM0')
    ap.add_argument('command', choices=['info', 'burn', 'reset'])
    ap.add_argument('image', nargs='?', help='固件镜像 (.img)')
    args = ap.parse_args()

    upg = AicUpg(args.port)
    if not upg.connect():
        sys.exit('未连接到设备：请让板子进入烧写模式（空片上电自动进入，或按住升级键复位）')
    upg.log('已连接（BROM UART 升级模式）')

    if args.command == 'info':
        upg.log(f"硬件信息: {run_with_resync(upg, upg.get_hwinfo)}")
    elif args.command == 'burn':
        if not args.image:
            ap.error('burn 需要镜像路径')
        run_with_resync(upg, burn, upg, args.image)
    elif args.command == 'reset':
        run_with_resync(upg, upg.run_shell, 'reset')
        upg.log('已发送复位命令')


if __name__ == '__main__':
    main()
