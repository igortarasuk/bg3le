import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Data;

import java.io.FileWriter;
import java.io.PrintWriter;

public class InspectRef2 extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_inspect_ref2.txt";
    static final long REF_ADDR = 0x07a16998L;
    static final long STR_ADDR = 0x01ce7f00L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Address refAddr = toAddr(REF_ADDR);
            MemoryBlock block = currentProgram.getMemory().getBlock(refAddr);
            out.println("refAddr=" + refAddr + " block=" + (block != null ? block.getName() + " [" + block.getStart() + "-" + block.getEnd() + "] perms r=" + block.isRead() + " w=" + block.isWrite() + " x=" + block.isExecute() + " initialized=" + block.isInitialized() : "null"));

            Instruction ins = getInstructionAt(refAddr);
            out.println("instructionAt refAddr: " + (ins != null ? ins.toString() : "none"));
            Data data = getDataAt(refAddr);
            out.println("dataAt refAddr: " + (data != null ? data.toString() : "none"));

            long ghidraLong = 0;
            try { ghidraLong = getLong(refAddr); } catch (Exception e) { out.println("getLong failed: " + e.getMessage()); }
            out.println("getLong(refAddr) via Ghidra memory = 0x" + Long.toHexString(ghidraLong));

            // print reference details
            ReferenceManager rm = currentProgram.getReferenceManager();
            for (Reference r : rm.getReferencesFrom(refAddr)) {
                out.println("refFrom " + refAddr + " -> " + r.getToAddress() + " type=" + r.getReferenceType() + " isPrimary=" + r.isPrimary());
            }
            // also check references TO refAddr itself (maybe something points at this data slot)
            for (Reference r : rm.getReferencesTo(refAddr)) {
                out.println("refTo " + refAddr + " <- " + r.getFromAddress() + " type=" + r.getReferenceType());
            }

            out.println();
            out.println("Dumping 64 bytes around refAddr via Ghidra memory:");
            for (int i = -16; i < 48; i++) {
                Address a = refAddr.add(i);
                try {
                    byte b = getByte(a);
                    out.println(a + ": 0x" + String.format("%02x", b & 0xff));
                } catch (Exception e) {
                    out.println(a + ": <unreadable>");
                }
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
