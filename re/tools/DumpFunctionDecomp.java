import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DumpFunctionDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            throw new IllegalArgumentException("Usage: <function-address> <output-path>");
        }

        Address address = toAddr(args[0]);
        Function function = currentProgram.getFunctionManager().getFunctionContaining(address);
        if (function == null) {
            throw new IllegalArgumentException("No function contains " + address);
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter output = new PrintWriter(args[1])) {
            output.println("/* " + function.getName() + " @ " +
                function.getEntryPoint() + " */");
            DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
            if (result.decompileCompleted()) {
                output.println(result.getDecompiledFunction().getC());
            } else {
                output.println("/* decompilation failed */");
            }
        } finally {
            decompiler.dispose();
        }
    }
}
