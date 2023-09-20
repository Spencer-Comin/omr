/*******************************************************************************
 * Copyright IBM Corp. and others 2023
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "default_compiler.hpp"
#include "Jit.hpp"

#include <readline/readline.h>
#include <readline/history.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>

extern "C" int validateExpr(int count, int key) {
    int nesting = 0;
    bool unmatched = false;
    for (int i = 0; i < rl_end; i++) {
        char c = rl_line_buffer[i];
        if (c == '(') nesting++;
        else if (c == ')') nesting--;
        if (nesting < 0) unmatched = true;
    }

    // if parens are balanced and cursor at end, accept
    if ((nesting == 0) && (rl_point == rl_end) && !unmatched) {
        rl_done = true;
        printf("\n");
    } else {
        rl_insert_text("\n");
        rl_redisplay();
    }

    return 0;
}

void initializeREPL() {
    if (rl_bind_key('\n', validateExpr) != 0) {
        fprintf(stderr, "FAIL: readline failed to bind newline\n");
        exit(-1);
    }
    if (rl_bind_key('\r', validateExpr) != 0){
        fprintf(stderr, "FAIL: readline failed to bind carriage return\n");
        exit(-1);
    }

}

char *tryReadExpr() {
    char *line = readline("> ");

    if (line != NULL)
        add_history(line);

    return line;
}


int main(int argc, char const * const * const argv) {
    bool initialized = initializeJit();
    if (!initialized) {
        fprintf(stderr, "FAIL: could not initialize JIT\n");
        exit(-1);
    }

    initializeREPL();


    char *line;
    while(true) {
        line = tryReadExpr();
        if (line == NULL)
            break;

        if (strlen(line) != 0) {
            ASTNode *tree = parseString(line);
            Tril::DefaultCompiler compiler(tree);
            int rc = compiler.compile();
            if (rc != 0) {
                fprintf(stderr,"FAIL: compilation error %d\n", rc);
                exit(-2);
            }
        }

        free(line); // readline returns malloc'd strings
    }

    shutdownJit();
    return 0;
}
