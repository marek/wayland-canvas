-- The editor in the README's pictures: Neovim's own colours, line numbers
-- and a status line; no plugins, and nothing of the user's config is read.
vim.o.termguicolors = true
vim.o.background = "dark"
vim.cmd.colorscheme("default")
vim.cmd.syntax("on")
vim.o.number = true
vim.o.cursorline = true
vim.o.signcolumn = "yes"
vim.o.laststatus = 3
vim.o.showmode = true
vim.o.swapfile = false
vim.o.wrap = false
vim.o.scrolloff = 4
vim.o.tabstop = 4
vim.o.shiftwidth = 4
vim.o.textwidth = 72
vim.o.title = true
vim.o.titlestring = "%t — Neovim"
