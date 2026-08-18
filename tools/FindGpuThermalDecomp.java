import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindGpuThermalDecomp extends GhidraScript {
    private static final long[] TARGETS = {
        0x00ad0a90L, 0x00ad0a94L, 0x00ad0a98L,
        0x00ad0a9cL, 0x00ad0aa0L, 0x00ad0aa4L
    };

    private static boolean isTarget(long value) {
        for (long target : TARGETS) {
            if (value == target) {
                return true;
            }
        }
        return false;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("Output path required");
        }

        Set<Function> functions = new LinkedHashSet<>();
        InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            for (int operand = 0; operand < instruction.getNumOperands(); operand++) {
                for (Object object : instruction.getOpObjects(operand)) {
                    if (object instanceof Scalar && isTarget(((Scalar) object).getUnsignedValue())) {
                        Function function = currentProgram.getFunctionManager()
                            .getFunctionContaining(instruction.getAddress());
                        println(instruction.getAddress() + ": " + instruction + " in " + function);
                        if (function != null) {
                            functions.add(function);
                        }
                    }
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter output = new PrintWriter(args[0])) {
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
