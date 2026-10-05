# Native project format 5

Version 5 keeps the version 4 SQL layout and all source, mask, profile, selection and placement semantics. New saves use schema 5; readers still accept schemas 1–4 with their original record rules.

Revisable raster records now use `version:3`. Each operation adds a sixth array element containing exactly three point arrays, in red, green, blue order:

`[kind,value,[inputBlack,inputWhite,gamma,outputBlack,outputWhite],masterPoints,[warmth,tint],[redPoints,greenPoints,bluePoints]]`

Each curve, including the master, contains 2–16 finite bounded points with strictly increasing input coordinates, input endpoints 0 and 1, and output coordinates in [0,1]. Non-Curves operations require identity master and channel controls. Stacks still contain 1–16 operations. Records are limited to 64 KiB each; aggregate layer parameter data remains limited to 4 MiB. Source and mask assets retain the previous aggregate 100 MP limits and streamed storage behavior.

Processing policy 2 applies the master curve and then each corresponding RGB curve in encoded straight RGB before conversion and one final canonical quantization. Identity channels preserve the legacy master-only result. Policy 1 remains supported only when all RGB channels are identity curves, retaining legacy processing. Unchanged legacy stacks preserve their policy. Newly revised stacks use policy 2.

A schema 4 adjustment record must still be version 2 and policy 1, with exactly five operation fields. Its missing channel curves materialize as identities. Schema 4 cannot admit a version 3 record or channel fields. Schema 5 accepts only version 3 records and policies 1 or 2; saving an unchanged legacy document upgrades the record envelope while preserving policy 1 and exact effective pixel identity.

Only retained source and mask tiles are stored. Derived caches are reevaluated before publication; cache UUID/revision are restored, and duplicate cache identities require identical source, ordered parameters, revision and policy. Layer sampling remains independent of cache pixel identity. Unsupported policies, malformed canonical JSON, mismatched sources, conflicting identities, cancellation and resource refusal fail without publishing a document or replacing a previous destination.
