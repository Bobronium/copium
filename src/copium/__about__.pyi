"""Version information."""

from typing import Literal, NamedTuple, TypeVar, Final, Generic

NameT = TypeVar("NameT")
EmailT = TypeVar("EmailT")

class Author(NamedTuple, Generic[NameT, EmailT]):
    name: NameT
    email: EmailT

class VersionInfo(NamedTuple):
    major: int
    minor: int
    patch: int
    prerelease: str | None
    build: int | None
    build_hash: str

__version__: str
__version_tuple__: VersionInfo
__commit_id__: str | None
__build_hash__: str

__authors__: Final[tuple[Author[Literal["Arseny Boykov (Bobronium)"], Literal["hi@bobronium.me"]]]]
