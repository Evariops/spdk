# 0014 — GCCP data-plane facts (SPEC-75, contrat gelé W0)

> Contrat normatif : `spdk-csi/docs/RPC-CONTRACT.md` (gelé 2026-07-13). Cette PR (stackée
> sur #17 seeded-rebuild) livre les faits data-plane que le control-plane GCCP consomme.
> La suite de conformité de modèle (SimDataPlane, `spdk-csi` W1) et ce fork sont testés par
> les MÊMES scénarios — une divergence est une CI rouge.

## Incréments (un commit/patch par item, dans cet ordre)

| # | Item | Contrat | Véhicule |
|---|---|---|---|
| 0014.1 ✅ | Epochs noncés : `bdev_cbt_epoch_open(nonce, generation)` ; nonce dans get_bdevs | §5 | module/bdev/cbt (arbre) |
| 0014.2 ✅ | `truncated=true` au resize sous epoch ouvert (phase OPEN incluse) | §5/T-D6 | module/bdev/cbt |
| 0014.3 ✅ | `bdev_cbt_epoch_close(epoch_id, mode: consumed\|preserve)` — consumed exige un token SUCCEEDED-verified local (-EPERM sinon ; consultation registre effective depuis patch 0015) | §4 | module/bdev/cbt |
| 0014.4 ✅ (patch 0014) | Incarnation : `bdev_raid_create(..., incarnation)` ; `expected_incarnation` sur tous les RPC engageants ⇒ `-ESTALE` | §1 | patch raid |
| 0014.5 ✅ (patch 0015) | Registre d'outcomes process-wide (TTL 15 min) : `{token, state RUNNING\|VERIFYING\|SUCCEEDED\|FAILED\|DIVERGENT\|CANCELED, bytes, verified, finished_at}` + `bdev_raid_get_rebuild_outcomes(token?)` ; contrat CANCELED (destroy pendant rebuild ⇒ zéro écriture après retour) ; `epoch_close(consumed)` consulte le registre | §2 | patch raid |
| 0014.6 ✅ (patch 0017) | get_bdevs enrichi par membre : `{state, since, content_generation, view_epoch, epoch_nonce, epoch_state, truncated}` (pont `vbdev_cbt_query_latest_epoch`) | §3 | patch raid+cbt |
| 0014.7 ✅ (patch 0016) | SB étendu : `content_generation` + `view_epoch` par membre, MÊME transaction que l'état (éjection = gen++ survivants ; complétion/skip = adoption du max) ; reassembly étendue ; SB minor 0→1 | §8/V-2 | patch raid (0001 discipline) |
| 0014.8 | Verify intégrée au rebuild : K=64 fenêtres échantillonnées sous quiesce_range, re-copie+re-verify avant DIVERGENT ; alimente le registre (VERIFYING) | §10a | patch raid |
| 0014.9 | Enveloppes : caps rebuild/verify/relocate ×(nominal, maintenance) + bornes de concurrence par node ; pose par RPC ; défaut 0=illimité ⇒ l'échec de pose est un incident CP | §6 | patch raid/rpc |
| 0014.10 | Epoch automatique à l'éjection d'un membre (nonce fourni au create ou généré+rapporté) | §12 (échéance W3) | patch raid+cbt |
| 0014.11 | Contrat pause H10 × rebuild : la pause ne fait JAMAIS échouer un rebuild en vol ; + force-resume au chemin de fence (DÉC-11) | §13 | patch nvmf (0003) |
| 0014.12 | `bdev_raid_verify_ranges(name, ranges\|allocated, rate_class=verify)` | §10b (W6) | patch raid |

Hors périmètre de cette PR : 0014b (enforcement PR) = wrapper C# côté spdk-csi (T-A5 —
vanilla `bdev_nvme_send_cmd` suffit), livré W3.

## Build

Label CI `build-images` sur la PR ⇒ image `v26.01-prNN-<sha>` à pinner dans spdk-csi.
