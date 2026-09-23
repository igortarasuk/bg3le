import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.program.model.mem.*;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

// Read-only run (-readOnly): clears bogus noreturn flags in memory only.
public class DecompileClientGate extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_client_gate.txt";
    static final long[] CLEAR_NORETURN = {0x07ed7090L, 0x02377170L, 0x0398e8e0L, 0x02480220L};
    static final long[] DECOMP = {0x0398e8e0L, 0x03867780L, 0x03867800L, 0x03867890L,
                                  0x04119c40L, 0x03867230L, 0x063be2caL, 0x02d2cb20L};
    static final String[] STRINGS = {"eocnet::AchievementMessage", "eocnet::NETMSG_ACHIEVEMENT_UNLOCKED_MESSAGE",
        "eocnet::AchievementProgressMessage", "unlock_achievement", "increment_achievement", "AchievementID"};

    DecompInterface decomp;
    PrintWriter out;
    Set<Address> done = new HashSet<>();

    void dumpFn(Function f) throws Exception {
        if (f == null) { out.println("  <no function>"); return; }
        out.println("Function: " + f.getName() + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses()
                + " noreturn=" + f.hasNoReturn());
        out.print("Callees:");
        for (Function c : f.getCalledFunctions(monitor)) out.print(" " + c.getName());
        out.println();
        if (done.contains(f.getEntryPoint())) { out.println("  (already decompiled above)"); return; }
        done.add(f.getEntryPoint());
        DecompileResults res = decomp.decompileFunction(f, 240, monitor);
        if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
        else out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
        out.println();
    }

    void disasmAround(Address a, int before, int after) {
        out.println("Disassembly around " + a + ":");
        Listing l = currentProgram.getListing();
        Address start = a.subtract(before);
        InstructionIterator it = l.getInstructions(start, true);
        int n = 0;
        while (it.hasNext() && n < 60) {
            Instruction ins = it.next();
            if (ins.getAddress().compareTo(a.add(after)) > 0) break;
            out.println("  " + ins.getAddress() + "  " + ins);
            n++;
        }
    }

    @Override
    public void run() throws Exception {
        out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            for (long a : CLEAR_NORETURN) {
                Function f = getFunctionAt(toAddr(a));
                if (f != null) { out.println("clear noreturn: " + f.getName() + " was " + f.hasNoReturn()); f.setNoReturn(false); }
            }
            out.println();
            for (long a : DECOMP) {
                out.println("=================================================================");
                out.println("=== target 0x" + Long.toHexString(a) + " (raw VMA 0x" + Long.toHexString(a - 0x100000L) + ") ===");
                Function f = getFunctionContaining(toAddr(a));
                if (f == null) disasmAround(toAddr(a), 0x60, 0x60); else dumpFn(f);
            }
            out.println("=================================================================");
            out.println("=== DATA xref 0x12e8d10 -> FUN_03867580: surrounding pointers ===");
            Memory mem = currentProgram.getMemory();
            Address base = toAddr(0x012e8d10L);
            for (int i = -6; i <= 6; i++) {
                Address p = base.add(i * 8L);
                long v = mem.getLong(p);
                Function tf = getFunctionAt(toAddr(v));
                Symbol s = getSymbolAt(toAddr(v));
                out.println("  " + p + ": 0x" + Long.toHexString(v) + (tf != null ? "  " + tf.getName() : "") + (s != null ? "  sym=" + s.getName() : ""));
            }
            for (String s : STRINGS) {
                out.println("=================================================================");
                out.println("=== string \"" + s + "\" ===");
                List<Address> hits = new ArrayList<>();
                byte[] pat = (s + "\0").getBytes("ASCII");
                Address a = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
                while (a != null && hits.size() < 4) { hits.add(a); a = mem.findBytes(a.add(1), pat, null, true, monitor); }
                for (Address h : hits) {
                    out.println(" at " + h);
                    ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(h);
                    int n = 0;
                    while (it.hasNext() && n < 12) {
                        Reference r = it.next(); n++;
                        Function cf = getFunctionContaining(r.getFromAddress());
                        out.println("   ref from " + r.getFromAddress() + " " + r.getReferenceType()
                                + " in " + (cf != null ? cf.getName() + "@" + cf.getEntryPoint() : "<none>"));
                    }
                    if (n == 0) out.println("   (no references recorded)");
                }
            }
            // decompile functions referencing the two most specific strings
            for (String s : new String[]{"eocnet::AchievementMessage", "eocnet::NETMSG_ACHIEVEMENT_UNLOCKED_MESSAGE", "unlock_achievement"}) {
                byte[] pat = (s + "\0").getBytes("ASCII");
                Address a = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
                if (a == null) continue;
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                int n = 0;
                while (it.hasNext() && n < 3) {
                    Reference r = it.next();
                    Function cf = getFunctionContaining(r.getFromAddress());
                    if (cf == null) continue;
                    n++;
                    out.println("=================================================================");
                    out.println("=== function referencing \"" + s + "\" ===");
                    dumpFn(cf);
                }
            }
        } finally { out.close(); }
        println("Done, wrote " + OUT_PATH);
    }
}
