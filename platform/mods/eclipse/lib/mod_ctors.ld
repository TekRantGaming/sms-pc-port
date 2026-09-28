/* Partial link of the code mods (-r): their static constructors, which the
 * port runs when the modules load (sms_mod_start), not at process start. */
SECTIONS
{
	sms_mod_ctors : { *(.ctors) *(SORT(.ctors.*)) }
}
