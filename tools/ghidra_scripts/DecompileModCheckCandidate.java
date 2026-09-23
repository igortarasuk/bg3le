// Second attempt: the previous run assumed the READ xref at raw 0x2e06bab
// belonged to the function starting at the preceding `ret` (0x2e06590), but
// that function's body range ([02f066f3,02f06ab3]) actually ENDS just before
// 0x2f06bab -- so the real comparison instructions live in a distinct
// function/region Ghidra hadn't identified as a Function at all. This script
// disassembles/decompiles starting exactly at the xref address itself and
// also tries createFunction() there, plus dumps a plain disassembly window
// so we can see the real boundaries by eye if decompilation still fails.

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileModCheckCandidate extends GhidraScript {

    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modcheck_candidate.txt";

    static final long IMAGE_BASE = 0x100000L;
    static final long XREF_ADDR = 0x2e06bab + IMAGE_BASE;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        FunctionManager fm = currentProgram.getFunctionManager();

        Address xref = space.getAddress(XREF_ADDR);
        out.println("XREF_ADDR = " + xref);

        Function containing = fm.getFunctionContaining(xref);
        out.println("getFunctionContaining(xref) = " + containing);

        out.println();
        out.println("--- plain disassembly window, 0x300 bytes before/after xref ---");
        Address start = xref.subtract(0x300);
        Address cur = start;
        Address end = xref.add(0x300);
        while (cur != null && cur.compareTo(end) < 0) {
            Instruction ins = currentProgram.getListing().getInstructionAt(cur);
            if (ins == null) {
                out.println("  " + cur + "  (no instruction / data)");
                cur = cur.add(1);
            } else {
                String marker = cur.equals(xref) ? "  <== XREF" : "";
                out.println("  " + cur + "  " + ins + marker);
                cur = cur.add(ins.getLength());
            }
        }

        Function f = null;

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        if (containing != null) {
            out.println();
            out.println("Decompiling containing function " + containing.getName() + " @ " + containing.getEntryPoint());
            DecompileResults res = decomp.decompileFunction(containing, 300, monitor);
            out.println("decompileCompleted(): " + res.decompileCompleted());
            if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("FAILED: " + res.getErrorMessage());
            }
        } else if (f != null) {
            out.println();
            out.println("Decompiling nearest preceding function " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults res = decomp.decompileFunction(f, 300, monitor);
            out.println("decompileCompleted(): " + res.decompileCompleted());
            if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("FAILED: " + res.getErrorMessage());
            }
        }

        out.close();
        println("DecompileModCheckCandidate: wrote " + OUT_PATH);
    }
}
