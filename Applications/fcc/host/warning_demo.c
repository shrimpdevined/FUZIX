/* Manual smoke test for the fcc warning system (not part of run.py).
 *
 * Compile with the freshly built compiler:
 *     fcc host/warning_demo.c -o /tmp/warning_demo.bin
 *
 * Expected diagnostics on stderr/stdout, with exit status still 0:
 *   <line>: warning: unused variable 'unused'
 *   <line>: warning: implicit declaration of function 'close'
 *   <line>: warning: suggest parentheses around assignment used as truth value
 *   <line>: warning: suggest parentheses around assignment used as truth value
 *   4 warnings generated.
 */
int main()
{
    int unused;
    int x;

    x = 0;
    if (x = 1) return 1;
    while (x = 0) return 2;
    close(0);
    return 0;
}
