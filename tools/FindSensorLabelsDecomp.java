import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;

public class FindSensorLabelsDecomp extends GhidraScript {
    private static boolean isSensorLabel(String value) {
        String lower = value.toLowerCase(Locale.ROOT);
        return lower.contains("hot spot") || lower.contains("hotspot") ||
            lower.contains("vrm") || lower.contains("[tmp #");
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) {
            throw new IllegalArgumentException("Output path required");
        }

        Set<Function> functions = new LinkedHashSet<>();
        try (PrintWriter output = new PrintWriter(args[0])) {
            DataIterator strings = currentProgram.getListing().getDefinedData(true);
            while (strings.hasNext() && !monitor.isCancelled()) {
                Data data = strings.next();
                Object valueObject = data.getValue();
                if (!(valueObject instanceof String)) {
                    continue;
                }

                String value = (String)valueObject;
                if (!isSensorLabel(value)) {
                    continue;
                }

                output.println("STRING " + data.getAddress() + " = " + value);
                ReferenceIterator references = currentProgram.getReferenceManager()
                    .getReferencesTo(data.getAddress());
                while (references.hasNext()) {
                    Reference reference = references.next();
                    Function function = currentProgram.getFunctionManager()
                        .getFunctionContaining(reference.getFromAddress());
                    output.println("  XREF " + reference.getFromAddress() + " in " + function);
                    if (function != null) {
                        functions.add(function);
                    }
                }
            }

            DecompInterface decompiler = new DecompInterface();
            decompiler.openProgram(currentProgram);
            try {
                for (Function function : functions) {
                    output.println();
                    output.println("/* " + function.getName() + " @ " +
                        function.getEntryPoint() + " */");
                    DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
                    if (result.decompileCompleted()) {
                        output.println(result.getDecompiledFunction().getC());
                    } else {
                        output.println("/* decompilation failed */");
                    }
                }
            } finally {
                decompiler.dispose();
            }
        }
    }
}
