/**
 * O avatar como lista de escolhas (pele, cabelo, roupa...), um numero por parte.
 * Quem sabe o que cada numero desenha e a dash (client/xbox/avatar_look.h); o
 * servidor so guarda a lista e confere o tamanho.
 */

export const LOOK_FIELDS = 16;
/** Folga para a dash ganhar opcoes sem o servidor mudar. */
const LOOK_MAX_VALUE = 31;

/** Igual a AVATAR_DEFAULT_LOOK em client/xbox/avatar_look.h: o avatar de toda conta nova. */
export const DEFAULT_LOOK: readonly number[] = [1, 1, 1, 0, 0, 1, 0, 1, 3, 0, 1, 0, 3, 1, 1, 0];

export function parseLook(value: unknown): number[] | null {
  if (!Array.isArray(value) || value.length !== LOOK_FIELDS) return null;
  for (const v of value) {
    if (!Number.isInteger(v) || v < 0 || v > LOOK_MAX_VALUE) return null;
  }
  return value as number[];
}
