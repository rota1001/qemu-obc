"""Capture real ADCS UART commands without replying; no web dependencies needed."""
import argparse
from pathlib import Path
import obcfi

root=Path(__file__).resolve().parents[4]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--firmware',type=Path,required=True,help='OBC100 BE-32 ELF file')
parser.add_argument('--qemu',type=Path,default=root/'qemu/build/qemu-system-arm')
args=parser.parse_args()
with obcfi.Obc(args.firmware,args.qemu) as obc:
    obc.wait_for_boot()
    print('ADCS socket:',obc.adcs.socket_path)
    after=0
    for command in ('ADCS_GET_TIME','ADCS_GET_Q','ADCS_SET_TIME 00563412'):
        print(obc.command(command).strip())
        record=obc.adcs.wait_for_request(after=after,timeout=5)
        print('Request :',record['request'].hex(' ').upper())
        print('Response:',record['response']) # None: deliberately no reply.
        after=record['id']
