// Root cause of the earlier "getFunctionContaining(xref) = null" result: the
// real IsModded/HasCustomMods-equivalent lives inside FUN_02f06590, but a
// call at 0x02f06aaf (to FUN_02538520, an sprintf-and-throw style helper) is
// misflagged by Ghidra as non-returning, so Ghidra never disassembled the
// ~0x1e0-byte fallthrough gap after it (0x02f06ab4..0x02f06b93) even though
// that code is real and reachable -- objdump's raw linear disassembly shows
// coherent instructions there, including our confirmed GUID-compare at
// 0x02f06bab. This script: (1) clears the no-return flag on FUN_02538520,
// (2) disassembles the gap, (3) recreates FUN_02f06590 so its body picks up
// the newly-connected code, (4) decompiles the result.

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.util.task.ConsoleTaskMonitor;

public class FixModCheckAnalysis extends GhidraScript {

    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_modcheck_candidate.txt";

    static final long FUNC_ENTRY = 0x02f06590L;
    static final long NORETURN_CALLEE = 0x02538520L;
    static final long GAP_START = 0x02f06ab4L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();

        Address calleeAddr = space.getAddress(NORETURN_CALLEE);
        Function callee = getFunctionAt(calleeAddr);
        out.println("Callee at " + calleeAddr + ": " + callee);
        if (callee != null) {
            out.println("  hasNoReturn() before: " + callee.hasNoReturn());
            callee.setNoReturn(false);
            out.println("  hasNoReturn() after: " + callee.hasNoReturn());
        }

        Address gapAddr = space.getAddress(GAP_START);
        out.println("Disassembling from gap start " + gapAddr);
        boolean disasmOk = disassemble(gapAddr);
        out.println("disassemble() returned: " + disasmOk);

        Address entry = space.getAddress(FUNC_ENTRY);
        Function existing = getFunctionAt(entry);
        out.println("Existing function at entry: " + existing);

        CreateFunctionCmd cmd = new CreateFunctionCmd(entry);
        boolean created = cmd.applyTo(currentProgram, monitor);
        out.println("CreateFunctionCmd.applyTo: " + created + " msg=" + cmd.getStatusMsg());

        Function func = getFunctionAt(entry);
        if (func == null) {
            out.println("Still no function at entry after recreate attempt.");
            out.close();
            return;
        }

        out.println("Function: " + func.getName() + " @ " + func.getEntryPoint());
        out.println("Body ranges:");
        func.getBody().forEach(r -> out.println("  " + r));

        Address xref = space.getAddress(0x02f06babL);
        out.println("Body contains xref (0x02f06bab)? " + func.getBody().contains(xref));

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor mon2 = new ConsoleTaskMonitor();

        out.println();
        out.println("-- decompiling with a 300s timeout --");
        DecompileResults res = decomp.decompileFunction(func, 300, mon2);
        out.println("decompileCompleted(): " + res.decompileCompleted());
        out.println("getErrorMessage(): [" + res.getErrorMessage() + "]");
        if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
            out.println("--- decompiled C ---");
            out.println(res.getDecompiledFunction().getC());
        } else {
            out.println("Decompile failed.");
        }

        out.close();
        println("FixModCheckAnalysis: wrote " + OUT_PATH);
    }
}
