// Diagnose why decompiling the three IsModded-search candidate functions
// (FUN_04f496f0, FUN_047de360, FUN_04797ec0) failed with an empty error
// message in FindIsModded.java. Checks: does Ghidra think this address is a
// real Function (vs. just a label)? How many instructions/basic blocks does
// it contain? Does decompilation succeed with a much longer timeout and, if
// it still fails, what does DecompileResults actually say (error message,
// timed-out flag, whether the high-level function was produced at all)?

import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.block.CodeBlock;
import ghidra.program.model.block.CodeBlockModel;
import ghidra.program.model.block.SimpleBlockModel;
import ghidra.program.model.listing.CodeUnitIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.util.task.ConsoleTaskMonitor;

public class DecompileDiagnose extends GhidraScript {

    static final String OUT_PATH =
        "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_decompile_diagnose.txt";

    static final long[] TARGETS = {0x4f496f0L, 0x47de360L, 0x4797ec0L};

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        FunctionManager fm = currentProgram.getFunctionManager();
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        for (long t : TARGETS) {
            Address addr = space.getAddress(t);
            out.println("======================================================================");
            out.println(String.format("Target 0x%x", t));
            out.println("======================================================================");

            Function func = fm.getFunctionContaining(addr);
            if (func == null) {
                out.println("No Function object contains this address at all.");
                out.println();
                continue;
            }

            Address entry = func.getEntryPoint();
            out.println("Function entry: " + entry + "  name=" + func.getName());
            out.println("Function body ranges:");
            func.getBody().forEach(r -> out.println("  " + r));

            long instrCount = 0;
            InstructionIterator it = currentProgram.getListing().getInstructions(func.getBody(), true);
            while (it.hasNext()) { it.next(); instrCount++; }
            out.println("Instruction count: " + instrCount);

            try {
                CodeBlockModel bbModel = new SimpleBlockModel(currentProgram);
                int bbCount = 0;
                var blocks = bbModel.getCodeBlocksContaining(func.getBody(), monitor);
                while (blocks.hasNext()) { blocks.next(); bbCount++; }
                out.println("Basic blocks touching body: " + bbCount);
            } catch (Exception e) {
                out.println("Basic block enumeration failed: " + e);
            }

            out.println();
            out.println("-- decompiling with a 300s timeout --");
            long t0 = System.currentTimeMillis();
            DecompileResults res = decomp.decompileFunction(func, 300, monitor);
            long elapsed = System.currentTimeMillis() - t0;
            out.println("elapsed: " + elapsed + " ms");
            out.println("decompileCompleted(): " + res.decompileCompleted());
            out.println("getErrorMessage(): [" + res.getErrorMessage() + "]");
            out.println("timedOut? (heuristic elapsed>=300000): " + (elapsed >= 300000));
            if (res.getFunction() != null) {
                out.println("high function produced: yes, proto=" + res.getFunction().getPrototypeString(false, false));
            } else {
                out.println("high function produced: no");
            }
            if (res.decompileCompleted() && res.getDecompiledFunction() != null) {
                out.println("--- decompiled C ---");
                out.println(res.getDecompiledFunction().getC());
            }
            out.println();
        }

        out.close();
        println("DecompileDiagnose: wrote " + OUT_PATH);
    }
}
