import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

// Decompile-only pass after HygienePredicateFull; appends to its output.
public class PredicateDecompile extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_predicate_full.txt";
    // Pairs: address of interest, guessed entry (0 = derive).
    static final long[][] TARGETS = {
        {0x038675f0L, 0}, {0x03867580L, 0}, {0x02538520L, 0}, {0x0239fa70L, 0},
        {0x03867780L, 0}, {0x03867800L, 0}, {0x03867230L, 0}, {0x04119c40L, 0}, {0x02d2cb20L, 0},
        {0x0460c8d0L, 0}, {0x027ab5d0L, 0}, {0x05209d00L, 0}, {0x0761d440L, 0}, {0x0398e8e0L, 0},
        {0x063be2caL, 0x063be100L}, {0x063c04f3L, 0x063bf260L}, {0x051db42cL, 0x051daf40L},
        {0x06386239L, 0x063815e0L}, {0x06ff93a0L, 0x06ff8bd0L},
        {0x02d27ae0L, 0}, {0x02dfec67L, 0}, {0x04385a9bL, 0}, {0x02f06590L, 0}, {0x031c647dL, 0},
        {0x03de3530L, 0}, {0x04dd16a0L, 0}, {0x04082150L, 0}, {0x04162850L, 0}, {0x03f2a290L, 0},
        {0x06ba0ce0L, 0}, {0x06761f70L, 0}, {0x066620d0L + 0x100000L, 0}, {0x03b7c3d0L, 0},
    };
    static final String[] STRINGS = {"eocnet::AchievementMessage", "eocnet::NETMSG_ACHIEVEMENT_UNLOCKED_MESSAGE",
        "eocnet::AchievementProgressMessage", "BG3_Quest01", "BG3_Quest02", "STEAMUSERSTATS_INTERFACE_VERSION012",
        "unlock_achievement", "LoadAchievementsDisabled"};

    DecompInterface decomp;
    PrintWriter out;
    Set<Address> decompiled = new HashSet<>();

    Function repair(Address entry) throws Exception {
        Function f = getFunctionAt(entry);
        if (f == null) {
            disassemble(entry);
            new CreateFunctionCmd(entry).applyTo(currentProgram, monitor);
            f = getFunctionAt(entry);
            if (f == null) { out.println("  could not create function at " + entry); return null; }
            out.println("  created function " + f.getName() + " at " + entry);
        }
        for (int pass = 0; pass < 12; pass++) {
            boolean changed = false;
            InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
            List<Address> gaps = new ArrayList<>();
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getFlowOverride() == FlowOverride.CALL_RETURN) {
                    boolean calleeReturns = true;
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isCall()) continue;
                        Function cf = getFunctionAt(r.getToAddress());
                        if (cf != null && cf.hasNoReturn()) calleeReturns = false;
                    }
                    if (calleeReturns) { ins.setFlowOverride(FlowOverride.NONE); changed = true; }
                }
                if (ins.getFlowType().isCall() && ins.getFlowOverride() != FlowOverride.CALL_RETURN) {
                    boolean noret = false;
                    for (Reference r : ins.getReferencesFrom()) {
                        if (!r.getReferenceType().isCall()) continue;
                        Function cf = getFunctionAt(r.getToAddress());
                        if (cf != null && cf.hasNoReturn()) noret = true;
                    }
                    if (!noret) {
                        Address ft = ins.getMaxAddress().add(1);
                        if (getInstructionAt(ft) == null && getUndefinedDataAt(ft) != null && (getByte(ft) & 0xff) != 0xcc) gaps.add(ft);
                    }
                }
                if (!ins.getFlowType().isCall()) {
                    for (Address d : ins.getFlows()) {
                        if (d == null || !currentProgram.getMemory().contains(d)) continue;
                        if (getInstructionAt(d) == null && getUndefinedDataAt(d) != null) gaps.add(d);
                    }
                }
            }
            for (Address g : gaps) { if (getInstructionAt(g) == null) { disassemble(g); changed = true; } }
            if (!changed) break;
            CreateFunctionCmd cmd = new CreateFunctionCmd(null, entry, null, SourceType.DEFAULT, false, true);
            if (!cmd.applyTo(currentProgram, monitor)) {
                currentProgram.getFunctionManager().removeFunction(entry);
                new CreateFunctionCmd(entry).applyTo(currentProgram, monitor);
            }
            f = getFunctionAt(entry);
            if (f == null) return null;
        }
        return f;
    }

    void dumpFn(Function f, boolean disasm) throws Exception {
        out.println("Function: " + f.getName() + " @ " + f.getEntryPoint() + " (raw 0x"
                + Long.toHexString(f.getEntryPoint().getOffset() - 0x100000L) + ") size=" + f.getBody().getNumAddresses()
                + " noreturn=" + f.hasNoReturn());
        out.print("Callees:");
        for (Function c : f.getCalledFunctions(monitor)) out.print(" " + c.getName() + "@" + c.getEntryPoint() + (c.hasNoReturn() ? "(noreturn)" : ""));
        out.println();
        out.print("Callers:");
        int n = 0;
        for (Function c : f.getCallingFunctions(monitor)) { out.print(" " + c.getName()); if (++n > 30) { out.print(" ..."); break; } }
        out.println();
        if (decompiled.contains(f.getEntryPoint())) { out.println("  (already decompiled above)"); return; }
        decompiled.add(f.getEntryPoint());
        if (disasm) {
            out.println("--- disassembly ---");
            InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
            while (it.hasNext()) { Instruction ins = it.next(); out.println("  " + ins.getAddress() + "  " + ins); }
        }
        out.println("--- decompile ---");
        DecompileResults res = decomp.decompileFunction(f, 300, monitor);
        if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
        else out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
        out.println();
    }

    List<Address> findPointers(long value) throws Exception {
        List<Address> hits = new ArrayList<>();
        Memory mem = currentProgram.getMemory();
        byte[] pat = new byte[8];
        for (int i = 0; i < 8; i++) pat[i] = (byte) ((value >>> (8 * i)) & 0xff);
        for (MemoryBlock b : mem.getBlocks()) {
            if (!b.isInitialized() || b.isExecute()) continue;
            Address a = mem.findBytes(b.getStart(), b.getEnd(), pat, null, true, monitor);
            while (a != null && hits.size() < 40) {
                hits.add(a);
                if (a.add(8).compareTo(b.getEnd()) >= 0) break;
                a = mem.findBytes(a.add(8), b.getEnd(), pat, null, true, monitor);
            }
        }
        return hits;
    }

    void describe(Address a) {
        Function tf = getFunctionContaining(a);
        Symbol s = getSymbolAt(a);
        MemoryBlock b = currentProgram.getMemory().getBlock(a);
        out.print(" -> " + a + (b != null ? "[" + b.getName() + "]" : "") + (tf != null ? " in " + tf.getName() + "@" + tf.getEntryPoint() : "") + (s != null ? " sym=" + s.getName() : ""));
    }

    void stringChase(String s) throws Exception {
        Memory mem = currentProgram.getMemory();
        byte[] pat = (s + "\0").getBytes("ASCII");
        Address str = mem.findBytes(currentProgram.getMinAddress(), pat, null, true, monitor);
        out.println("=================================================================");
        out.println("=== string \"" + s + "\" at " + str + " ===");
        if (str == null) return;
        ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(str);
        int n = 0;
        while (rit.hasNext()) { Reference r = rit.next(); n++; Function cf = getFunctionContaining(r.getFromAddress());
            out.println("  ref from " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (cf != null ? cf.getName() + "@" + cf.getEntryPoint() : "<none>")); }
        if (n == 0) out.println("  (no Ghidra references recorded)");
        List<Address> ptrs = findPointers(str.getOffset());
        out.println("  raw 8-byte pointers to string: " + ptrs.size());
        for (Address p : ptrs) {
            out.print("  ptr at"); describe(p); out.println();
            for (int k = -4; k <= 4; k++) {
                Address q = p.add(8L * k);
                try {
                    long v = mem.getLong(q);
                    Address va = toAddr(v);
                    MemoryBlock vb = mem.getBlock(va);
                    String what = vb == null ? "" : (vb.isExecute() ? " code" : " " + vb.getName());
                    Function vf = vb != null && vb.isExecute() ? getFunctionContaining(va) : null;
                    String strv = "";
                    if (vb != null && !vb.isExecute()) { try { byte[] sb = new byte[32]; mem.getBytes(va, sb); int e = 0; while (e < 32 && sb[e] >= 0x20 && sb[e] < 0x7f) e++; if (e > 3) strv = " \"" + new String(sb, 0, e, "ASCII") + "\""; } catch (Exception ex) {} }
                    out.println("     " + (k == 0 ? "*" : " ") + q + ": 0x" + Long.toHexString(v) + what + (vf != null ? " " + vf.getName() : "") + strv);
                } catch (Exception ex) { }
            }
        }
    }

    void vtableRefs(long vt) {
        out.println("=================================================================");
        out.println("=== references to vtable " + toAddr(vt) + " ===");
        ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(toAddr(vt));
        int n = 0;
        while (rit.hasNext()) { Reference r = rit.next(); n++; Function cf = getFunctionContaining(r.getFromAddress());
            out.println("  ref from " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (cf != null ? cf.getName() + "@" + cf.getEntryPoint() : "<none>")); }
        if (n == 0) out.println("  (none)");
    }

    @Override
    public void run() throws Exception {
        out = new PrintWriter(new FileWriter(OUT_PATH, true));
        try {
            decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            out.println();
            out.println("#################################################################");
            out.println("### PredicateDecompile pass ###");
            Memory mem = currentProgram.getMemory();
            out.println("=================================================================");
            out.println("=== OFFICIAL-MODULE FIXEDSTRING TABLE (ghidra 0x7e5af30.., raw 0x7d5af30..) ===");
            for (long a = 0x7e5af30L; a <= 0x7e5aff0L; a += 4) {
                Address p = toAddr(a);
                Symbol s = getSymbolAt(p);
                MemoryBlock b = mem.getBlock(p);
                ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(p);
                StringBuilder refs = new StringBuilder();
                int n = 0;
                while (rit.hasNext() && n < 12) { Reference r = rit.next(); n++; Function cf = getFunctionContaining(r.getFromAddress());
                    refs.append(" ").append(r.getFromAddress()).append(r.getReferenceType().isWrite() ? "(W" : "(R").append(cf != null ? "," + cf.getName() : "").append(")"); }
                out.println("  " + p + " (raw 0x" + Long.toHexString(a - 0x100000L) + ") block=" + (b != null ? b.getName() : "?")
                        + (s != null ? " sym=" + s.getName() : "") + " refs:" + refs);
            }
            out.println();

            Map<Long, Function> resolved = new LinkedHashMap<>();
            for (long[] t : TARGETS) {
                Address a = toAddr(t[0]);
                Function f = getFunctionContaining(a);
                if (f == null && t[1] != 0) f = repair(toAddr(t[1]));
                if (f == null) { Function bf = getFunctionBefore(a); if (bf != null) f = repair(bf.getEntryPoint()); }
                if (f == null) { out.println("target " + a + ": unresolved"); continue; }
                if (!f.getBody().contains(a)) out.println("target " + a + ": using nearest function " + f.getName() + " (body does not contain target)");
                resolved.put(t[0], f);
            }
            out.println("=================================================================");
            out.println("=== DECOMPILES ===");
            for (long[] t : TARGETS) {
                Function f = resolved.get(t[0]);
                out.println("=================================================================");
                out.println("=== target 0x" + Long.toHexString(t[0]) + " (raw 0x" + Long.toHexString(t[0] - 0x100000L) + ") ===");
                if (f == null) { out.println("  <unresolved>"); continue; }
                boolean disasm = t[0] == 0x038675f0L || t[0] == 0x03867580L;
                dumpFn(f, disasm);
            }

            out.println("=================================================================");
            out.println("=== XREFS after repair ===");
            for (long a : new long[]{0x038675f0L, 0x03867580L, 0x02d27ae0L, 0x02f06590L, 0x06ba0ce0L, 0x06761f70L}) {
                out.println("refs to " + toAddr(a) + ":");
                ReferenceIterator rit = currentProgram.getReferenceManager().getReferencesTo(toAddr(a));
                while (rit.hasNext()) { Reference r = rit.next(); Function cf = getFunctionContaining(r.getFromAddress());
                    out.println("  from " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (cf != null ? cf.getName() + "@" + cf.getEntryPoint() : "<none>")); }
            }
            out.println();
            vtableRefs(0x07a29bb8L);
            vtableRefs(0x07a29bf8L);
            for (String s : STRINGS) stringChase(s);
        } finally { out.close(); }
        println("Done, appended to " + OUT_PATH);
    }
}
