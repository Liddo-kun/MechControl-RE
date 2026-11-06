import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class DumpCallersDecomp extends GhidraScript {
    private void collectCallers(Address target, int depth, Set<Function> functions) {
        if (depth < 1) {
            return;
        }
        ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(target);
        while (references.hasNext()) {
            Reference reference = references.next();
            Function caller = currentProgram.getFunctionManager()
                .getFunctionContaining(reference.getFromAddress());
            if (caller != null && functions.add(caller)) {
                println(reference.getFromAddress() + " -> " + target + " in " + caller);
                collectCallers(caller.getEntryPoint(), depth - 1, functions);
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3) {
            throw new IllegalArgumentException("Usage: <target-address> <depth> <output-path>");
        }

        Address target = toAddr(args[0]);
        int depth = Integer.parseInt(args[1]);
        Set<Function> functions = new LinkedHashSet<>();
        collectCallers(target, depth, functions);

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter output = new PrintWriter(args[2])) {
            for (Function function : functions) {
                output.println("/* " + function.getName() + " @ " + function.getEntryPoint() + " */");
                DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                if (result.decompileCompleted()) {
                    output.println(result.getDecompiledFunction().getC());
                } else {
                    output.println("/* decompilation failed */");
                }
                output.println();
            }
        } finally {
            decompiler.dispose();
        }
    }
}
