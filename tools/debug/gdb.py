#!/usr/bin/env python3

import asyncio


feature_xml = """<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target>
<feature name="org.gnu.gdb.m68k.core">
  <reg name="d0" bitsize="32"/>
  <reg name="d1" bitsize="32"/>
  <reg name="d2" bitsize="32"/>
  <reg name="d3" bitsize="32"/>
  <reg name="d4" bitsize="32"/>
  <reg name="d5" bitsize="32"/>
  <reg name="d6" bitsize="32"/>
  <reg name="d7" bitsize="32"/>
  <reg name="a0" bitsize="32" type="data_ptr"/>
  <reg name="a1" bitsize="32" type="data_ptr"/>
  <reg name="a2" bitsize="32" type="data_ptr"/>
  <reg name="a3" bitsize="32" type="data_ptr"/>
  <reg name="a4" bitsize="32" type="data_ptr"/>
  <reg name="a5" bitsize="32" type="data_ptr"/>
  <reg name="fp" bitsize="32" type="data_ptr"/>
  <reg name="sp" bitsize="32" type="data_ptr"/>

  <reg name="ps" bitsize="16"/>
  <reg name="pc" bitsize="32" type="code_ptr"/>
</feature>
</target>"""


class GdbConnection():
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer

    @staticmethod
    def chksum(packet):
        return sum(ord(c) for c in packet) & 255

    async def ack(self):
        # read a character
        data = await self.reader.read(1)
        print("(gdb) <- {}".format(data))

        # fail if not acknowledge and not retransmission marker
        if data not in [b'+', b'-']:
            raise ValueError(data)

        return data == b'+'

    async def wait(self):
        data = await self.reader.read(1)
        print("(gdb) <- {}".format(data))
        assert data == b'\x03'

    async def recv(self):
        # read first character
        data = await self.reader.read(1)

        # end of stream ?
        if not data:
            return ''

        # packet start marker ?
        if data != b'$':
            raise ValueError(data)

        # read packet and decode it
        raw_packet = await self.reader.readuntil(b'#')
        packet = raw_packet.decode()[:-1]
        raw_chksum = await self.reader.read(2)
        chksum = int(raw_chksum.decode(), 16)
        print("(gdb) <- '{}'".format(packet))

        # check if check sum matches
        if chksum != self.chksum(packet):
            self.send_nack()
            return None

        return packet

    def send_nack(self, packet=None):
        print("(gdb) -> '-'")
        self.writer.write(b'-')
        if packet is not None:
            self.send(packet)

    def send_ack(self, packet=None):
        print("(gdb) -> '+'")
        self.writer.write(b'+')
        if packet is not None:
            self.send(packet)

    def send(self, packet):
        data = '${}#{:02x}'.format(packet, self.chksum(packet))
        print("(gdb) -> '{}'".format(packet))
        self.writer.write(data.encode())


class GdbStub():
    __regs__ = ['D0', 'D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7',
                'A0', 'A1', 'A2', 'A3', 'A4', 'A5', 'A6', 'A7',
                'SR', 'PC']

    def __init__(self, gdb, uae):
        self.gdb = gdb
        self.uae = uae
        self.bp_set = set()

    async def handle_query(self, packet):
        if packet == 'C':
            # Return the current thread ID.
            self.gdb.send_ack('QC0')
        elif packet == 'Offsets':
            # Get section offsets that the target used when relocating
            # the downloaded image.
            entry = await self.uae.entry_point()
            if entry:
                self.gdb.send_ack('TextSeg={:08x}'.format(entry))
            else:
                self.gdb.send_ack('')
        elif packet.startswith('Supported'):
            self.gdb.send_ack(
                    'PacketSize=4096;qXfer:features:read+;hwbreak+')
        elif packet.startswith('Xfer:features:read:'):
            self.gdb.send_ack('l' + feature_xml)
        elif packet == 'TStatus':
            # Is there a trace experiment running right now?
            self.gdb.send_ack('T0')
        elif packet == 'TfV' or packet == 'TfP':
            self.gdb.send_ack('')
        elif packet == 'fThreadInfo':
            # Obtain a list of all active thread IDs from the target
            # (OS). The first query of the sequence will be the
            # 'qfThreadInfo' query; subsequent queries in the sequence
            # will be the 'qsThreadInfo' query.
            self.gdb.send_ack('m0')
        elif packet == 'sThreadInfo':
            # lower case letter 'L' denotes end of list
            self.gdb.send_ack('l')
        elif packet == 'Attached':
            # Return an indication of whether the remote server
            # attached to an existing process or created a new process.
            # ‘1’: The remote server attached to an existing process.
            self.gdb.send_ack('1')
        elif packet == 'Symbol::':
            # Notify the target that GDB is prepared to serve symbol
            # lookup requests.
            self.gdb.send_ack('OK')

        return True

    async def handle_packet(self, packet):
        if packet == '?':
            # Indicate the reason the target halted.
            self.gdb.send_ack('S05')
        if packet == '!':
            self.gdb.send_ack('OK')
        elif packet[0] == 'q':
            # Packets starting with ‘q’ are general query packets.
            return await self.handle_query(packet[1:])
        elif packet[0] == 'D':
            self.gdb.send_ack('OK')
            self.uae.resume()
            raise asyncio.CancelledError
        elif packet[0] == 'H':
            # Set thread for subsequent operations
            op = packet[1]
            tid = int(packet[2:])
            self.gdb.send_ack('OK')
        elif packet == 'g':
            # Read general registers.
            regs = await self.uae.read_registers()
            dump = ''.join(regs.as_hex(name) for name in self.__regs__)
            self.gdb.send_ack(dump)
        elif packet == 'c':
            # Continue.
            self.uae.resume()
            self.gdb.send_ack()
            return False
        elif packet == 's':
            # Step instruction
            self.uae.step()
            self.gdb.send_ack()
            return False
        elif packet[0] == 'p':
            # Read the value of register n; n is in hex.
            self.gdb.send_ack('DEAD{:04X}'.format(int(packet[1:], 16)))
        elif packet[0] == 'm':
            # Read length addressable memory units starting at address
            # addr.
            addr, length = map(lambda x: int(x, 16), packet[1:].split(','))
            self.gdb.send_ack(await self.uae.read_memory(addr, length))
        elif packet == 'vCont?':
            self.gdb.send_ack('vCont:')
        elif packet.startswith('z0'):
            # Remove a software breakpoint at address _addr_ of type _kind_.
            addr, kind = packet[3:].split(',')
            if addr in self.bp_set:
                self.bp_set.remove(addr)
                await self.uae.remove_hwbreak(int(addr, 16))
                self.gdb.send_ack('OK')
            else:
                self.gdb.send_ack('E01')
        elif packet.startswith('Z0'):
            # Insert a software breakpoint at address _addr_ of type _kind_.
            addr, kind = packet[3:].split(',')
            if addr not in self.bp_set:
                self.bp_set.add(addr)
                await self.uae.insert_hwbreak(int(addr, 16))
                self.gdb.send_ack('OK')
            else:
                self.gdb.send_ack('E01')
        elif packet[0] == 'X':
            # Write data to memory. Memory is specified by its address addr and
            # number of addressable memory units length.
            # TODO
            self.gdb.send_ack('OK')
        elif packet[0] == 'v':
            # The correct reply to an unknown 'v' packet is to return the
            # empty string.
            self.gdb.send_ack('')

        # TODO
        # {G XX…XX}
        #   Write general registers.
        # {M addr,length:XX…}
        #   Write length addressable memory units starting at address addr.
        # {P n…=r…}
        #   Write register n… with value r….
        # {R XX}
        #   Restart the program being debugged. The XX is ignored.

        return True

    async def run(self):
        assert await self.gdb.ack()
        self.uae.interrupt()
        stopdata = await self.uae.prologue()

        running = True

        while running:
            interactive = True

            while interactive:
                packet = await self.gdb.recv()
                if not packet:
                    return

                interactive = await self.handle_packet(packet)
                if interactive:
                    assert await self.gdb.ack()

            uae_stop = asyncio.create_task(self.uae.prologue())
            gdb_wait = asyncio.create_task(self.gdb.wait())

            done, pending = await asyncio.wait(
                    {uae_stop, gdb_wait}, return_when=asyncio.FIRST_COMPLETED)

            if uae_stop in done:
                print('FS-UAE stopped!')
                gdb_wait.cancel()
                try:
                    await gdb_wait
                except asyncio.CancelledError:
                    pass
                stopdata = uae_stop.result()
            else:
                print('GDB sent break message!')
                self.uae.interrupt()
                stopdata = await uae_stop
                if False:
                    uae_stop.cancel()
                    try:
                        stopdata = await uae_stop
                    except asyncio.CancelledError:
                        self.uae.interrupt()
                        stopdata = await self.uae.prologue()

            # Process stop information.
            regs = stopdata['regs']
            dump = ';'.join('{:x}:{}'.format(num, regs.as_hex(name))
                            for num, name in enumerate(self.__regs__))
            self.gdb.send('T05;{};'.format(dump))
            await self.gdb.ack()
