extern "C" __declspec(dllimport) int NamedProbe();
extern "C" __declspec(dllimport) int OrdinalProbe();
// Linked for static PE inspection only; no product or test harness executes it.
int main() { return NamedProbe() + OrdinalProbe(); }
