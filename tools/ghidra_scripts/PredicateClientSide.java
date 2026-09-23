import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.*;

// Creates and decompiles the client achievement path; appends output.
public class PredicateClientSide extends GhidraScript {
    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_predicate_full.txt";
    // Pairs: address of interest, guessed entry (0 = scan back).
    static final long[][] TARGETS = {
        {0x035b2607L, 0x035b2440L}, {0x05178630L, 0x05178630L}, {0x06ba1170L, 0x06ba1170L},
        {0x06ba11e0L, 0x06ba11e0L}, {0x076bdf30L, 0x076bdf30L}, {0x076bda80L, 0x076bda80L},
        {0x06ba0afaL, 0}, {0x06ba0ce0L, 0x06ba0ce0L}, {0x03967890L, 0x03967890L},
    };

    DecompInterface decomp;
    PrintWriter out;

    Address scanBack(Address a) throws Exception {
        Address cur = a;
        for (int i = 0; i < 0x4000; i++) {
            cur = cur.subtract(1);
            if ((getByte(cur) & 0xff) == 0xcc && (getByte(cur.subtract(1)) & 0xff) == 0xcc) return cur.add(1);
        }
        return null;
    }

    Function repair(Address entry) throws Exception {
        Function f = getFunctionAt(entry);
        if (f == null) {
            Function cont = getFunctionContaining(entry);
            if (cont != null) out.println("  entry " + entry + " lies inside " + cont.getName() + "; removing it");
            if (cont != null) currentProgram.getFunctionManager().removeFunction(cont.getEntryPoint());
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
        out.println("  function " + f.getName() + " body=" + f.getBody().getNumAddresses() + " max=" + f.getBody().getMaxAddress());
        return f;
    }

    void dumpFn(Function f) throws Exception {
        out.println("Function: " + f.getName() + " @ " + f.getEntryPoint() + " (raw 0x"
                + Long.toHexString(f.getEntryPoint().getOffset() - 0x100000L) + ") size=" + f.getBody().getNumAddresses());
        out.print("Callees:");
        for (Function c : f.getCalledFunctions(monitor)) out.print(" " + c.getName() + "@" + c.getEntryPoint());
        out.println();
        out.print("Callers:");
        for (Function c : f.getCallingFunctions(monitor)) out.print(" " + c.getName() + "@" + c.getEntryPoint());
        out.println();
        out.println("--- decompile ---");
        DecompileResults res = decomp.decompileFunction(f, 300, monitor);
        if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
        else out.println("decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
        out.println();
    }

    @Override
    public void run() throws Exception {
        out = new PrintWriter(new FileWriter(OUT_PATH, true));
        try {
            decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            out.println();
            out.println("#################################################################");
            out.println("### PredicateClientSide pass (client handler, manager, Steam wrapper) ###");
            for (long[] t : TARGETS) {
                Address a = toAddr(t[0]);
                out.println("=================================================================");
                out.println("=== target 0x" + Long.toHexString(t[0]) + " (raw 0x" + Long.toHexString(t[0] - 0x100000L) + ") ===");
                Address entry = t[1] != 0 ? toAddr(t[1]) : null;
                if (entry == null) {
                    Function cont = getFunctionContaining(a);
                    if (cont != null) entry = cont.getEntryPoint();
                    else { entry = scanBack(a); out.println("  scanned-back entry: " + entry); }
                }
                if (entry == null) { out.println("  no entry"); continue; }
                Function f = repair(entry);
                if (f == null) continue;
                if (!f.getBody().contains(a)) out.println("  NOTE: body does not contain address of interest");
                dumpFn(f);
            }
        } finally { out.close(); }
        println("Done, appended to " + OUT_PATH);
    }
}
