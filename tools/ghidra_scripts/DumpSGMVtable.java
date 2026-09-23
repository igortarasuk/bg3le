import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpSGMVtable extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_sgm_vtable.txt";
    static final long[] CANDIDATES = { 0x021cc554L, 0x00df2a0cL, 0x07aeb840L };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            for (long c : CANDIDATES) {
                Address a = toAddr(c);
                out.println("=== around " + a + " ===");
                out.println("block=" + currentProgram.getMemory().getBlock(a));
                for (int i = -8; i <= 12; i++) {
                    Address slot = a.add((long) i * 8);
                    long val = 0;
                    try { val = getLong(slot); } catch (Exception e) { out.println(slot + ": <unreadable>"); continue; }
                    Function f = null;
                    try { f = getFunctionAt(toAddr(val)); } catch (Exception e) {}
                    out.println(slot + ": 0x" + Long.toHexString(val) + (f != null ? "  -> " + f.getName() : ""));
                }
                out.println();
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
