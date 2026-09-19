#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sim_device_pc.py — PC 侧"手机/设备"模拟器（面板的遥控端）
队伍 421 / laoshuxiwantou，轨 C（协议层 + 设备模拟器）

与面板 sim/sm_net.c 走同一套冻结协议（详见 sim/protocol.md）：
行式 JSON，'\\n' 结尾，UTF-8。本脚本模拟手机 App：发 set/get 命令，
后台线程持续接收并打印面板的 ack / evt:state / evt:rule / evt:devices。

两种模式的演示场景
==================
1) TCP 模式（Linux 模拟器演示拓扑）——面板固件跑在 openvela 模拟器里，
   sm_net_start_tcp(9000) 监听本机回环；PC 上运行本脚本连接 127.0.0.1:9000，
   模拟手机 App 经"局域网"下发控制命令（当前无 WiFi 驱动，以回环呈现
   同一协议代码路径）：

       python3 sim_device_pc.py --host 127.0.0.1 --port 9000

2) 串口模式（黄山派板端演示拓扑）——板端 sm_net_start_uart("/dev/ttyS1",
   115200)，经 USB 转串口线接到 PC；PC 运行本脚本模拟手机侧：

       python3 sim_device_pc.py --serial /dev/ttyUSB1 --baud 115200
       （缺 pyserial 时会提示 pip install pyserial 并退出）

REPL 命令
=========
  light1 on 1            -> {"cmd":"set","dev":"light1","prop":"on","value":1}
  light1 brightness 80   -> {"cmd":"set","dev":"light1","prop":"brightness","value":80}
  sensor1 temp 305       -> {"cmd":"set","dev":"sensor1","prop":"temp","value":305}
  get                    -> {"cmd":"get"}，回设备列表
  demo                   -> 每 2 秒播放脚本序列: 开灯→调亮度→开插座→关灯，
                            最后 get 一次展示终态
  {..}                   -> 原样发送任意一行 JSON（调试/畸形用例测试）
  quit / exit / q        -> 退出
"""

import argparse
import json
import socket
import sys
import threading
import time

# demo 命令的脚本化序列（每步 2 秒）：开灯→调亮度→开插座→关灯
DEMO_SEQ = [
    ("light1", "on", 1),
    ("light1", "brightness", 80),
    ("socket1", "on", 1),
    ("light1", "on", 0),
]

PROPS = ("on", "brightness", "temp")   # 协议冻结的属性枚举

HELP_TEXT = """可用命令:
  <dev> <prop> <value>   例: light1 on 1 / light1 brightness 80 / sensor1 temp 305
  get                    查询设备列表
  demo                   每 2 秒播放: 开灯→调亮度→开插座→关灯
  {json}                 原样发送一行 JSON（可发畸形用例测试 ack ok:0）
  quit                   退出"""


# ---------------------------------------------------------------------------
# 收包与展示
# ---------------------------------------------------------------------------

def handle_line(line):
    """解析面板下行消息并打印（后台接收线程调用）"""
    line = line.strip()
    if not line:
        return

    try:
        msg = json.loads(line)
    except ValueError:
        print(f"[<-] 非JSON行: {line!r}")
        return

    evt = msg.get("evt")
    if evt == "ack":
        ok = msg.get("ok")
        extra = "" if ok else f' err={msg.get("err")}'
        print(f"[<-] ack ok={ok}{extra}")
    elif evt == "state":
        print(f'[<-] 状态推送 {msg.get("dev")}.{msg.get("prop")} = {msg.get("value")}')
    elif evt == "rule":
        print(f'[<-] 规则命中 id={msg.get("id")} name={msg.get("name")!r}: '
              f'{msg.get("dev")}.{msg.get("prop")} = {msg.get("value")}')
    elif evt == "devices":
        print("[<-] 设备列表:")
        for dev in msg.get("devices", []):
            print("      " + json.dumps(dev, ensure_ascii=False))
    else:
        print(f"[<-] {line}")


def build_set(dev, prop, value):
    """构造 set 命令行（紧凑 JSON，UTF-8）"""
    msg = {"cmd": "set", "dev": dev, "prop": prop, "value": value}
    return json.dumps(msg, ensure_ascii=False, separators=(",", ":"))


# ---------------------------------------------------------------------------
# 传输层
# ---------------------------------------------------------------------------

class TcpTransport:
    """TCP 模式：面板监听，本脚本作客户端连接（Linux 模拟器演示拓扑）"""

    def __init__(self, host, port, retries=40, delay=0.25):
        last = None
        sock = None
        for _ in range(retries):            # 宽限面板启动/烧录重启时间
            try:
                sock = socket.create_connection((host, port), timeout=2)
                break
            except OSError as exc:
                last = exc
                time.sleep(delay)
        if sock is None:
            raise SystemExit(f"[!] 无法连接 {host}:{port}: {last}")

        sock.settimeout(None)               # 收包线程改用阻塞读
        self.sock = sock
        print(f"[ok] 已连接面板 {host}:{port} (TCP)")

    def send_line(self, line):
        try:
            self.sock.sendall((line + "\n").encode("utf-8"))
        except OSError as exc:
            print(f"[!] 发送失败（连接已断开？）: {exc}")

    def recv_loop(self, on_line):
        buf = b""
        try:
            while True:
                data = self.sock.recv(4096)
                if not data:
                    print("\n[!] 面板连接已断开")
                    return
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    on_line(raw.decode("utf-8", "replace"))
        except OSError as exc:
            print(f"\n[!] 连接读取错误: {exc}")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class SerialTransport:
    """串口模式：板端 UART 演示拓扑（依赖 pyserial）"""

    def __init__(self, devpath, baud):
        try:
            import serial
        except ImportError:
            print("[!] 缺少 pyserial，请先安装后重试:  pip install pyserial")
            raise SystemExit(1)

        self._serial = serial
        self.ser = serial.Serial(devpath, baud, timeout=0.3)
        print(f"[ok] 串口已打开 {devpath} @ {baud} (UART)")

    def send_line(self, line):
        try:
            self.ser.write((line + "\n").encode("utf-8"))
        except self._serial.SerialException as exc:
            print(f"[!] 串口发送失败: {exc}")

    def recv_loop(self, on_line):
        try:
            while True:
                raw = self.ser.readline()   # 以 '\n' 分帧，超时返回空
                if raw:
                    on_line(raw.decode("utf-8", "replace"))
        except self._serial.SerialException as exc:
            print(f"\n[!] 串口读取错误: {exc}")

    def close(self):
        try:
            self.ser.close()
        except self._serial.SerialException:
            pass


def start_reader(transport):
    """后台线程持续收面板下行消息"""
    th = threading.Thread(target=transport.recv_loop, args=(handle_line,),
                          daemon=True)
    th.start()
    return th


# ---------------------------------------------------------------------------
# REPL 交互
# ---------------------------------------------------------------------------

def run_demo(transport):
    """脚本化演示序列：每 2 秒一步，结束后 get 展示终态"""
    print("[demo] 播放脚本序列（每步 2 秒）: 开灯→调亮度→开插座→关灯")
    for dev, prop, value in DEMO_SEQ:
        transport.send_line(build_set(dev, prop, value))
        time.sleep(2)
    transport.send_line('{"cmd":"get"}')
    print("[demo] 结束")


def repl(transport):
    print(HELP_TEXT)
    while True:
        try:
            raw = input("sim> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return

        if not raw:
            continue

        low = raw.lower()
        if low in ("quit", "exit", "q"):
            return

        if low == "demo":
            run_demo(transport)
            continue

        if low == "get":
            transport.send_line('{"cmd":"get"}')
            continue

        if raw.startswith("{"):             # 原样透传（含畸形用例）
            transport.send_line(raw)
            continue

        parts = raw.split()
        if len(parts) == 3 and parts[1] in PROPS:
            try:
                value = int(parts[2])
            except ValueError:
                print("[!] value 必须是整数，例: light1 on 1")
                continue
            transport.send_line(build_set(parts[0], parts[1], value))
            continue

        print("[!] 无法识别。格式: <dev> <prop> <value> | get | demo | quit")


def main():
    ap = argparse.ArgumentParser(
        description="智能家居面板 PC 侧模拟器（模拟手机 App，遥控行式 JSON 协议）")
    ap.add_argument("--host", default="127.0.0.1",
                    help="面板 TCP 地址（默认 127.0.0.1，Linux 模拟器演示）")
    ap.add_argument("--port", type=int, default=9000,
                    help="面板 TCP 端口（默认 9000）")
    ap.add_argument("--serial", default=None,
                    help="串口设备路径（如 /dev/ttyUSB1），指定后走 UART 模式（板端演示）")
    ap.add_argument("--baud", type=int, default=115200,
                    help="串口波特率（默认 115200）")
    args = ap.parse_args()

    transport = (SerialTransport(args.serial, args.baud) if args.serial
                 else TcpTransport(args.host, args.port))

    start_reader(transport)
    try:
        repl(transport)
    finally:
        transport.close()


if __name__ == "__main__":
    main()
