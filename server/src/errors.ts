/** An error that is safe to show to the player. Anything else becomes a generic 500. */
export class GameError extends Error {
  constructor(
    readonly code: string,
    message: string,
    readonly status = 400,
  ) {
    super(message);
  }
}
