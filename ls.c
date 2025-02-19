#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <inttypes.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "httpd.h"

#define LS_ALLOC_SIZE (4 * 4096)

#ifdef USE_THREADS
static pthread_mutex_t lock_dircache = PTHREAD_MUTEX_INITIALIZER;
#endif

/* --------------------------------------------------------- */

#define CACHE_SIZE 32

#ifdef PRINT_OWNER
static char *xgetpwuid(uid_t uid)
{
	static char           *cache[CACHE_SIZE];
	static uid_t          uids[CACHE_SIZE];
	static unsigned int   used,next;

	struct passwd *pw;
	int i;

	if (do_chroot)
		return NULL; /* would'nt work anyway .. */

	for (i = 0; i < used; i++) {
		if (uids[i] == uid)
			return cache[i];
	}

	/* 404 */
	pw = getpwuid(uid);
	if (NULL != cache[next]) {
		free(cache[next]);
		cache[next] = NULL;
	}
	if (NULL != pw)
		cache[next] = strdup(pw->pw_name);
	uids[next] = uid;
	if (debug)
		fprintf(stderr,"uid: %3d  n=%2d, name=%s\n",
				(int)uid, next, cache[next] ? cache[next] : "?");

	next++;
	if (CACHE_SIZE == next) next = 0;
	if (used < CACHE_SIZE) used++;

	return pw ? pw->pw_name : NULL;
}

static char *xgetgrgid(gid_t gid)
{
	static char *cache[CACHE_SIZE];
	static gid_t gids[CACHE_SIZE];
	static unsigned int used, next;
	struct group *gr;
	int i;

	if (do_chroot)
		return NULL; /* would'nt work anyway .. */

	for (i = 0; i < used; i++) {
		if (gids[i] == gid)
			return cache[i];
	}

	/* 404 */
	gr = getgrgid(gid);
	if (NULL != cache[next]) {
		free(cache[next]);
		cache[next] = NULL;
	}
	if (NULL != gr)
		cache[next] = strdup(gr->gr_name);
	gids[next] = gid;
	if (debug)
		fprintf(stderr,"gid: %3d  n=%2d, name=%s\n",
				(int)gid,next,cache[next] ? cache[next] : "?");

	next++;
	if (CACHE_SIZE == next) next = 0;
	if (used < CACHE_SIZE) used++;

	return gr ? gr->gr_name : NULL;
}
#endif

/* --------------------------------------------------------- */

struct myfile {
	int r;
	struct stat s;
	char n[1];
};

static int compare_files(const void *a, const void *b)
{
	const struct myfile *aa = *(struct myfile**)a;
	const struct myfile *bb = *(struct myfile**)b;

	if (S_ISDIR(aa->s.st_mode) !=  S_ISDIR(bb->s.st_mode))
		return S_ISDIR(aa->s.st_mode) ? -1 : 1;
	return strcmp(aa->n,bb->n);
}

static char do_quote[256];

void init_quote(void)
{
	int i;
	for (i = 0; i < 256; i++)
		do_quote[i] = (isalnum(i) || ispunct(i)) ? 0 : 1;
	do_quote['+'] = 1;
	do_quote['#'] = 1;
	do_quote['%'] = 1;
	do_quote['"'] = 1;
	do_quote['?'] = 1;
}

char *quote(unsigned char *path, int maxlength)
{
	static unsigned char buf[2048]; /* FIXME: threads break this... */
	int i, j, n=strlen((char*)path);
	if (n > maxlength)
		n = maxlength;
	for (i=0, j=0; i<n && j<sizeof(buf)-4; i++, j++) {
		if (!do_quote[path[i]]) {
			buf[j] = path[i];
			continue;
		}
		sprintf((char*)buf+j,"%%%02x",path[i]);
		j += 2;
	}
	buf[j] = 0;
	return (char*)buf;
}

#if !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__APPLE__)
static void strmode(mode_t mode, char *dest)
{
	static const char *rwx[] = {
		"---","--x","-w-","-wx","r--","r-x","rw-","rwx" };

	/* file type */
	switch (mode & S_IFMT) {
	case S_IFIFO:  dest[0] = 'p'; break;
	case S_IFCHR:  dest[0] = 'c'; break;
	case S_IFDIR:  dest[0] = 'd'; break;
	case S_IFBLK:  dest[0] = 'b'; break;
	case S_IFREG:  dest[0] = '-'; break;
	case S_IFLNK:  dest[0] = 'l'; break;
	case S_IFSOCK: dest[0] = '='; break;
	default:       dest[0] = '?'; break;
	}

	/* access rights */
	sprintf(dest+1,"%s%s%s",
			rwx[(mode >> 6) & 0x7],
			rwx[(mode >> 3) & 0x7],
			rwx[mode        & 0x7]);
}
#endif

#define HTML5_PLAYER \
"<style> \n\
h1 { \n\
	margin-bottom: 0; \n\
} \n\
h2 { \n\
	margin: 0; \n\
} \n\
.audio-player { \n\
	display: inline-block; \n\
	vertical-align: middle; \n\
	margin: 0; \n\
	margin-right: 1em; \n\
} \n\
.gvolume { \n\
	font-family: monospace; \n\
	vertical-align: middle; \n\
	margin: 0; \n\
	margin-right: 1em; \n\
} \n\
.audio-volume { \n\
	display: inline-block; \n\
	vertical-align: middle; \n\
	width: 5em; \n\
	margin: 0; \n\
	margin-right: 1em; \n\
} \n\
.link-pbutton, .link-qbutton-alt { \n\
	font-family: inherit; \n\
	font-size: inherit; \n\
	font-style: inherit; \n\
	font-weight: inherit; \n\
	display: inline-block; \n\
	vertical-align: middle; \n\
	background-color: transparent; \n\
	border: none; \n\
	color: #9900ff; \n\
	cursor: pointer; \n\
	text-decoration: none; \n\
	margin-right: 1em; \n\
	padding: 0; \n\
} \n\
.link-qbutton { \n\
	font-family: inherit; \n\
	font-size: inherit; \n\
	font-style: inherit; \n\
	font-weight: inherit; \n\
	display: inline-block; \n\
	vertical-align: middle; \n\
	background-color: transparent; \n\
	border: none; \n\
	color: #9900ff; \n\
	cursor: pointer; \n\
	text-decoration: none; \n\
	margin-left: 1em; \n\
	margin-right: 1em; \n\
	padding: 0; \n\
} \n\
.sticky { \n\
	position: sticky; \n\
	top: 0; \n\
	z-index: 1000; \n\
	background-color: black; \n\
} \n\
</style> \n\
</style> \n\
<script> \n\
var queue = []; \n\
var queuebtn = []; \n\
var queuepbtn = []; \n\
var quemax = 0; \n\
var cque = 0; \n\
var cquemov = 0; \n\
var pidx = 0; \n\
var paused = []; \n\
var gvol; \n\
function formatTime(seconds) { \n\
	var minutes = Math.floor(seconds / 60); \n\
	var seconds = Math.floor(seconds % 60); \n\
	if (seconds < 10) \n\
		seconds = '0' + seconds; \n\
	return minutes + ':' + seconds; \n\
} \n\
function shuffleArray(array) { \n\
	for (var i = array.length - 1; i >= 0; i--) { \n\
		var j = Math.floor(Math.random() * (i + 1)); \n\
		var temp = array[i]; \n\
		array[i] = array[j]; \n\
		array[j] = temp; \n\
	} \n\
} \n\
document.addEventListener('DOMContentLoaded', function() { \n\
	var pallbutton, currbutton, ctrack, hparent; \n\
	const audiolinks = document.querySelectorAll('pre a[href$=\".flac\"],\
pre a[href$=\".mp3\"],\
pre a[href$=\".m4a\"],\
pre a[href$=\".opus\"],\
pre a[href$=\".ogg\"],\
pre a[href$=\".wav\"]'); \n\
	if (audiolinks.length > 0) { \n\
		const head1 = document.getElementsByTagName('hr')[0]; \n\
		hparent = head1.parentNode; \n\
		hparent.className = 'sticky'; \n\
		const head2 = document.createElement('h2'); \n\
		head1.parentNode.insertBefore(head2, head1.nextSibling); \n\
		pallbutton = document.createElement('button'); \n\
		pallbutton.className = 'link-qbutton-alt'; \n\
		pallbutton.textContent = 'Play'; \n\
		pallbutton.addEventListener('click', function() { \n\
			const pbuttons = document.getElementsByClassName('link-pbutton'); \n\
			var idx = 0; \n\
			for (var i = 0; i < pbuttons.length; i++) \n\
				if (pbuttons[i].textContent == 'Pause') { \n\
					pbuttons[i].click(); \n\
					paused[pidx++] = pbuttons[i]; \n\
					idx = 1; \n\
				} \n\
			if (!idx) { \n\
				for (var i = pidx-1; i >= 0; i--) \n\
					if (paused[i].textContent == 'Play') { \n\
						paused[i].click(); \n\
						idx = 1; \n\
						break; \n\
					} \n\
				pidx = 0; \n\
				if (!idx && quemax) \n\
					queuepbtn[cque].click(); \n\
			} else \n\
				pallbutton.textContent = 'Play'; \n\
		}); \n\
		head2.appendChild(pallbutton); \n\
		const prevbutton = document.createElement('button'); \n\
		prevbutton.className = 'link-qbutton-alt'; \n\
		prevbutton.textContent = '<<'; \n\
		prevbutton.addEventListener('click', function() { \n\
			if (cquemov || !quemax || cque - 1 < 0) \n\
				return; \n\
			if (pallbutton.textContent == 'Pause') { \n\
				const pbuttons = document.getElementsByClassName('link-pbutton'); \n\
				for (var i = 0; i < pbuttons.length; i++) \n\
					if (pbuttons[i].textContent == 'Pause') \n\
						pbuttons[i].click(); \n\
				const div = queue[cque]; \n\
				div.innerHTML = ''; \n\
				div.value = '0'; \n\
				if (queuepbtn[cque - 1].textContent == 'Play') { \n\
					queuepbtn[cque - 1].click(); \n\
					cquemov = -1; \n\
				} \n\
			} else { \n\
				const div = queue[cque]; \n\
				div.innerHTML = ''; \n\
				div.value = '0'; \n\
				pidx = 0; \n\
				cque--; \n\
				currbutton.textContent = '[' + cque + ']'; \n\
				ctrack.innerHTML = queuebtn[cque].previousSibling.innerHTML; \n\
			} \n\
		}); \n\
		head2.appendChild(prevbutton); \n\
		currbutton = document.createElement('button'); \n\
		currbutton.className = 'link-qbutton-alt'; \n\
		currbutton.textContent = '[0]'; \n\
		currbutton.addEventListener('click', function() { \n\
			if (!quemax) \n\
				return; \n\
			const div = queue[cque]; \n\
			const contentrect = div.getBoundingClientRect(); \n\
			const hprect = hparent.getBoundingClientRect(); \n\
			const scrollpos = window.scrollY + contentrect.top - (hprect.height + 10); \n\
			window.scrollTo({ top: scrollpos }); \n\
		}); \n\
		head2.appendChild(currbutton); \n\
		const nextbutton = document.createElement('button'); \n\
		nextbutton.className = 'link-qbutton-alt'; \n\
		nextbutton.textContent = '>>'; \n\
		nextbutton.addEventListener('click', function() { \n\
			if (cquemov || cque + 1 >= quemax) \n\
				return; \n\
			if (pallbutton.textContent == 'Pause') { \n\
				const pbuttons = document.getElementsByClassName('link-pbutton'); \n\
				for (var i = 0; i < pbuttons.length; i++) \n\
					if (pbuttons[i].textContent == 'Pause') \n\
						pbuttons[i].click(); \n\
				const div = queue[cque]; \n\
				div.innerHTML = ''; \n\
				div.value = '0'; \n\
				if (queuepbtn[cque + 1].textContent == 'Play') { \n\
					queuepbtn[cque + 1].click(); \n\
					cquemov = 1; \n\
				} \n\
			} else { \n\
				const div = queue[cque]; \n\
				div.innerHTML = ''; \n\
				div.value = '0'; \n\
				pidx = 0; \n\
				cque++; \n\
				currbutton.textContent = '[' + cque + ']'; \n\
				ctrack.innerHTML = queuebtn[cque].previousSibling.innerHTML; \n\
			} \n\
		}); \n\
		head2.appendChild(nextbutton); \n\
		const qallbutton = document.createElement('button'); \n\
		qallbutton.className = 'link-qbutton-alt'; \n\
		qallbutton.textContent = 'Enqueue'; \n\
		qallbutton.addEventListener('click', function() { \n\
			cque = 0; \n\
			const qbuttons = document.getElementsByClassName('link-qbutton'); \n\
			for (var i = 0; i < qbuttons.length; i++) \n\
				qbuttons[i].click(); \n\
		}); \n\
		head2.appendChild(qallbutton); \n\
		const qdelbutton = document.createElement('button'); \n\
		qdelbutton.className = 'link-qbutton-alt'; \n\
		qdelbutton.textContent = 'Dequeue'; \n\
		qdelbutton.addEventListener('click', function() { \n\
			cque = 0; \n\
			for (var i = 0; i < quemax; i++) \n\
				queuebtn[i].textContent = 'Enqueue'; \n\
			quemax = 0; \n\
		}); \n\
		head2.appendChild(qdelbutton); \n\
		const qrandbutton = document.createElement('button'); \n\
		qrandbutton.className = 'link-qbutton-alt'; \n\
		qrandbutton.textContent = 'Randqueue'; \n\
		qrandbutton.addEventListener('click', function() { \n\
			cque = 0; \n\
			const ibuf = []; \n\
			for (var i = 0; i < quemax; i++) \n\
				ibuf[i] = i; \n\
			shuffleArray(ibuf); \n\
			const _queue = queue.slice(0); \n\
			const _queuebtn = queuebtn.slice(0); \n\
			const _queuepbtn = queuepbtn.slice(0); \n\
			for (var i = 0; i < quemax; i++) { \n\
				queue[i] = _queue[ibuf[i]]; \n\
				queuebtn[i] = _queuebtn[ibuf[i]]; \n\
				queuepbtn[i] = _queuepbtn[ibuf[i]]; \n\
			} \n\
			for (var i = 0; i < quemax; i++) { \n\
				queuebtn[i].textContent = 'Dequeue[' + i + ']'; \n\
			} \n\
		}); \n\
		head2.appendChild(qrandbutton); \n\
		const gvolrange = document.createElement('input'); \n\
		gvolrange.className = 'gvolume'; \n\
		gvol = localStorage.getItem('volrange.value'); \n\
		if (!gvol) { \n\
			localStorage.setItem('volrange.value', '100'); \n\
			gvol = '100'; \n\
		} \n\
		gvolrange.value = gvol; \n\
		gvolrange.type = 'range'; \n\
		gvolrange.addEventListener('input', function() { \n\
			gvol = gvolrange.value; \n\
			localStorage.setItem('volrange.value', gvol); \n\
			const vols = document.getElementsByClassName('audio-volume'); \n\
			for (var i = 0; i < vols.length; i++) { \n\
				vols[i].value = gvol; \n\
				const event = new Event('input'); \n\
				vols[i].dispatchEvent(event); \n\
			} \n\
		}); \n\
		const hr1 = document.createElement('hr'); \n\
		const hr2 = document.createElement('hr'); \n\
		hr1.size = 1;\n\
		hr2.size = 1;\n\
		const desc = document.createElement('span'); \n\
		desc.className = 'gvolume'; \n\
		desc.textContent = 'Master'; \n\
		ctrack = document.createElement('span'); \n\
		ctrack.className = 'gvolume'; \n\
		ctrack.textContent = 'Track'; \n\
		head2.parentNode.insertBefore(hr1, head2.nextSibling); \n\
		head2.parentNode.insertBefore(desc, hr1.nextSibling); \n\
		head2.parentNode.insertBefore(gvolrange, desc.nextSibling); \n\
		head2.parentNode.insertBefore(ctrack, gvolrange.nextSibling); \n\
		head2.parentNode.insertBefore(hr2, ctrack.nextSibling); \n\
	} \n\
	audiolinks.forEach(link => { \n\
		const div = document.createElement('div'); \n\
		div.className = 'audio-player'; \n\
		div.value = '0'; \n\
		const pbutton = document.createElement('button'); \n\
		pbutton.className = 'link-pbutton'; \n\
		pbutton.textContent = 'Play'; \n\
		pbutton.addEventListener('click', function() { \n\
			if (div.value == '0') { \n\
				const audio = document.createElement('audio'); \n\
				const time = document.createElement('span'); \n\
				const seek = document.createElement('input'); \n\
				const dur = document.createElement('span'); \n\
				const vol = document.createElement('input'); \n\
				const loop = document.createElement('input'); \n\
				const looplabel = document.createElement('span'); \n\
				audio.controls = false; \n\
				audio.src = link.href; \n\
				audio.addEventListener('timeupdate', function() { \n\
					var value = (audio.currentTime / audio.duration) * 100; \n\
					seek.value = value; \n\
					time.textContent = formatTime(audio.currentTime); \n\
					dur.textContent = formatTime(audio.duration); \n\
				}); \n\
				audio.addEventListener('ended', function(){ \n\
					audio.currentTime = 0; \n\
					if (loop.checked) { \n\
						audio.play(); \n\
					} else { \n\
						pbutton.textContent = 'Play'; \n\
						if (cque + 1 < quemax) { \n\
							div.innerHTML = ''; \n\
							div.value = '0'; \n\
							queuepbtn[++cque].click(); \n\
						} else \n\
							cque = 0;\n\
						currbutton.textContent = '[' + cque + ']'; \n\
					} \n\
				}); \n\
				div.appendChild(audio); \n\
				time.className = 'audio-player'; \n\
				time.textContent = '0:00'; \n\
				div.appendChild(time); \n\
				seek.className = 'audio-player'; \n\
				seek.value = '0'; \n\
				seek.type = 'range'; \n\
				seek.addEventListener('input', function() { \n\
					if (pbutton.textContent != 'Loading') \n\
						audio.currentTime = (seek.value / 100) * audio.duration; \n\
				}); \n\
				div.appendChild(seek); \n\
				dur.className = 'audio-player'; \n\
				dur.textContent = '0:00'; \n\
				div.appendChild(dur); \n\
				vol.className = 'audio-volume'; \n\
				vol.value = gvol; \n\
				vol.type = 'range'; \n\
				audio.volume = vol.value / 100; \n\
				vol.addEventListener('input', function() { \n\
					audio.volume = vol.value / 100; \n\
				}); \n\
				div.appendChild(vol); \n\
				loop.className = 'audio-player'; \n\
				loop.type = 'checkbox'; \n\
				loop.name = 'loop'; \n\
				div.appendChild(loop); \n\
				looplabel.className = 'audio-player'; \n\
				looplabel.textContent = 'Loop'; \n\
				div.appendChild(looplabel); \n\
				div.value = '1'; \n\
				pbutton.textContent = 'Loading'; \n\
				const playpromise = audio.play(); \n\
				audio.onplaying = function() { \n\
					playpromise.then(_ => { \n\
						ctrack.innerHTML = link.innerHTML; \n\
						pbutton.textContent = 'Pause'; \n\
						cque += cquemov; \n\
						cquemov = 0; \n\
						currbutton.textContent = '[' + cque + ']'; \n\
						pallbutton.textContent = 'Pause'; \n\
					}) \n\
				}; \n\
			} else { \n\
				const audios = div.getElementsByTagName('audio'); \n\
				if (audios[0].paused) { \n\
					pbutton.textContent = 'Loading'; \n\
					const playpromise = audios[0].play(); \n\
					audios[0].onplaying = function() { \n\
						playpromise.then(_ => { \n\
							ctrack.innerHTML = link.innerHTML; \n\
							pbutton.textContent = 'Pause'; \n\
							cque += cquemov; \n\
							cquemov = 0; \n\
							currbutton.textContent = '[' + cque + ']'; \n\
							pallbutton.textContent = 'Pause'; \n\
						}) \n\
					}; \n\
				} else if (pbutton.textContent == 'Pause') { \n\
					audios[0].pause(); \n\
					pbutton.textContent = 'Play'; \n\
					currbutton.textContent = '[' + cque + ']'; \n\
				} \n\
			} \n\
		}); \n\
		const qbutton = document.createElement('button'); \n\
		qbutton.className = 'link-qbutton'; \n\
		qbutton.textContent = 'Enqueue'; \n\
		qbutton.addEventListener('click', function() { \n\
			var out = -1; \n\
			for (var i = 0; i < quemax; i++) { \n\
				if (queue[i] === div) { \n\
					out = i; \n\
					break; \n\
				} \n\
			} \n\
			if (out < 0) { \n\
				queuebtn[quemax] = qbutton; \n\
				queuepbtn[quemax] = pbutton; \n\
				queue[quemax] = div; \n\
				qbutton.textContent = 'Dequeue[' + quemax + ']'; \n\
				quemax++; \n\
			} else { \n\
				quemax--; \n\
				queuebtn.splice(out, 1); \n\
				queuepbtn.splice(out, 1); \n\
				queue.splice(out, 1); \n\
				qbutton.textContent = 'Enqueue'; \n\
				for (var i = 0; i < quemax; i++) { \n\
					queuebtn[i].textContent = 'Dequeue[' + i + ']'; \n\
				} \n\
			} \n\
		}); \n\
		link.parentNode.insertBefore(qbutton, link.nextSibling); \n\
		link.parentNode.insertBefore(pbutton, qbutton.nextSibling); \n\
		link.parentNode.insertBefore(div, pbutton.nextSibling); \n\
	}); \n\
}); \n\
</script>"

static char *ls(time_t now, char *hostname, char *filename, char *path, int *length)
{
	DIR *dir;
	struct dirent *file;
	struct myfile **files = NULL;
	struct myfile **re1;
	char *h1, *h2, *re2, *buf = NULL;
	int count, len, size, i, uid, gid;
	char line[1024];
	#ifdef PRINT_OWNER
	char *pw = NULL, *gr = NULL;
	#endif
	char html5_player[] = HTML5_PLAYER;

	if (debug)
		fprintf(stderr,"dir: reading %s\n",filename);
	if (NULL == (dir = opendir(filename)))
		return NULL;

	/* read dir */
	uid = getuid();
	gid = getgid();
	for (count = 0;; count++) {
		if (NULL == (file = readdir(dir)))
			break;
		if (0 == strcmp(file->d_name,".")) {
			/* skip the the "." directory */
			count--;
			continue;
		}
		if (0 == strcmp(path,"/") && 0 == strcmp(file->d_name,"..")) {
			/* skip the ".." directory in root dir */
			count--;
			continue;
		}

		if (0 == (count % 64)) {
			re1 = realloc(files,(count+64)*sizeof(struct myfile*));
			if (NULL == re1)
				goto oom;
			files = re1;
		}

		files[count] = malloc(strlen(file->d_name)+sizeof(struct myfile));
		if (NULL == files[count])
			goto oom;
		strcpy(files[count]->n,file->d_name);
		sprintf(line,"%s/%s",filename,file->d_name);
		if (-1 == stat(line,&files[count]->s)) {
			free(files[count]);
			count--;
			continue;
		}

		files[count]->r = 0;
		if (S_ISDIR(files[count]->s.st_mode) ||
			S_ISREG(files[count]->s.st_mode)) {
			if (files[count]->s.st_uid == uid &&
				files[count]->s.st_mode & 0400)
				files[count]->r = 1;
			else if (files[count]->s.st_uid == gid &&
					 files[count]->s.st_mode & 0040)
				files[count]->r = 1; /* FIXME: check additional groups */
			else if (files[count]->s.st_mode & 0004)
				files[count]->r = 1;
		}
	}
	closedir(dir);

	/* sort */
	if (count)
		qsort(files,count,sizeof(struct myfile*),compare_files);

	/* output */
	size = LS_ALLOC_SIZE;
	buf  = malloc(size);
	if (NULL == buf)
		goto oom;
	len  = 0;

	len += sprintf(buf+len, "<!DOCTYPE html>"
				"<html lang=\"en\">"
				"<head>"
				"<title>%s:%d%s</title>"
				"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />"
				"</head>\n"
				"<body bgcolor=#000000 text=#ffffff link=#0066ff vlink=#00ccff alink=#cc00ff>\n"
				"<div><h1>listing: \n",
				hostname,tcp_port,path);

	h1 = path, h2 = path+1;
	for (;;) {
		if (len > size)
			abort();
		if (len+(LS_ALLOC_SIZE>>2) > size) {
			size += LS_ALLOC_SIZE;
			re2 = realloc(buf,size);
			if (NULL == re2)
				goto oom;
			buf = re2;
		}
		len += sprintf(buf+len,"<a href=\"%s\">%*.*s</a>",
						quote((unsigned char*)path,h2-path),
						(int)(h2-h1),
						(int)(h2-h1),
						h1);
		h1 = h2;
		h2 = strchr(h2,'/');
		if (NULL == h2)
			break;
		h2++;
	}

	len += sprintf(buf+len, "</h1><hr size=1></div><pre>\n"
				#ifdef PRINT_OWNER
				"<b>access      user      group     date             "
				#else
				"<b>access      date             "
				#endif
				"size  name</b>\n\n");

	for (i = 0; i < count; i++) {
		if (len > size)
			abort();
		if (len+(LS_ALLOC_SIZE>>2) > size) {
			size += LS_ALLOC_SIZE;
			re2 = realloc(buf,size);
			if (NULL == re2)
				goto oom;
			buf = re2;
		}

		/* mode */
		strmode(files[i]->s.st_mode, buf+len);
		len += 10;
		buf[len++] = ' ';
		buf[len++] = ' ';

		/* user */
		#ifdef PRINT_OWNER
		pw = xgetpwuid(files[i]->s.st_uid);
		if (NULL != pw)
			len += sprintf(buf+len,"%-8.8s  ",pw);
		else
			len += sprintf(buf+len,"%8d  ",(int)files[i]->s.st_uid);

		/* group */
		gr = xgetgrgid(files[i]->s.st_gid);
		if (NULL != gr)
			len += sprintf(buf+len,"%-8.8s  ",gr);
		else
			len += sprintf(buf+len,"%8d  ",(int)files[i]->s.st_gid);
		#endif

		/* mtime */
		if (now - files[i]->s.st_mtime > 60*60*24*30*6)
			len += strftime(buf+len,255,"%b %d  %Y  ",
							gmtime(&files[i]->s.st_mtime));
		else
			len += strftime(buf+len,255,"%b %d %H:%M  ",
							gmtime(&files[i]->s.st_mtime));

		/* size */
		if (S_ISDIR(files[i]->s.st_mode)) {
			len += sprintf(buf+len,"  &lt;DIR&gt;  ");
		} else if (!S_ISREG(files[i]->s.st_mode)) {
			len += sprintf(buf+len,"     --  ");
		} else if (files[i]->s.st_size < 1024*9) {
			len += sprintf(buf+len,"%4d  B  ", (int)files[i]->s.st_size);
		} else if (files[i]->s.st_size < 1024*1024*9) {
			len += sprintf(buf+len,"%4d kB  ", (int)(files[i]->s.st_size>>10));
		} else if ((int64_t)(files[i]->s.st_size) < (int64_t)1024*1024*1024*9) {
			len += sprintf(buf+len,"%4d MB  ", (int)(files[i]->s.st_size>>20));
		} else if ((int64_t)(files[i]->s.st_size) < (int64_t)1024*1024*1024*1024*9) {
			len += sprintf(buf+len,"%4d GB  ", (int)(files[i]->s.st_size>>30));
		} else {
			len += sprintf(buf+len,"%4d TB  ", (int)(files[i]->s.st_size>>40));
		}

		/* filename */
		if (files[i]->r) {
			len += sprintf(buf+len,"<a href=\"%s%s\">%s</a>\n",
							quote((unsigned char*)files[i]->n,9999),
							S_ISDIR(files[i]->s.st_mode) ? "/" : "",
							files[i]->n);
		} else {
			len += sprintf(buf+len,"%s\n",files[i]->n);
		}
	}
	strftime(line,32,"%d/%b/%Y %H:%M:%S GMT",gmtime(&now));
	if (len + sizeof(html5_player) + 1024 > size) {
		size += sizeof(html5_player) + 1024;
		re2 = realloc(buf,size);
		if (NULL == re2)
			goto oom;
		buf = re2;
	}
	len += sprintf(buf+len, "</pre><hr size=1>\n"
				"<small>%s</small>\n"
				"</body>\n%s\n", line, html5_player);
	for (i = 0; i < count; i++)
		free(files[i]);
	if (count)
		free(files);

	/* return results */
	*length = len;
	return buf;

 oom:
	fprintf(stderr,"oom\n");
	if (files) {
		for (i = 0; i < count; i++)
			if (files[i])
				free(files[i]);
		free(files);
	}
	if (buf)
		free(buf);
	return NULL;
}

/* --------------------------------------------------------- */

#define MAX_CACHE_AGE   3600   /* seconds */

struct DIRCACHE *dirs = NULL;

void free_dir(struct DIRCACHE *dir)
{
	DO_LOCK(dir->lock_refcount);
	dir->refcount--;
	if (dir->refcount > 0) {
		DO_UNLOCK(dir->lock_refcount);
		return;
	}
	DO_UNLOCK(dir->lock_refcount);
	if (debug)
		fprintf(stderr,"dir: delete %s\n",dir->path);
	FREE_LOCK(dir->lock_refcount);
	FREE_LOCK(dir->lock_reading);
	FREE_COND(dir->wait_reading);
	if (NULL != dir->html)
		free(dir->html);
	free(dir);
}

struct DIRCACHE *get_dir(struct REQUEST *req, char *filename)
{
	struct DIRCACHE *this, *prev;
	int i;

	DO_LOCK(lock_dircache);
	for (prev = NULL, this = dirs, i=0; this != NULL;
		 prev = this, this = this->next, i++) {
		if (0 == strcmp(filename,this->path)) {
			/* remove from list */
			if (NULL == prev)
				dirs = this->next;
			else
				prev->next = this->next;
			if (debug)
				fprintf(stderr,"dir: found %s\n",this->path);
			break;
		}
		if (i > max_dircache) {
			/* reached cache size limit -> free last element */
#if 0
			if (this->next != NULL) {
				fprintf(stderr,"panic: this should'nt happen (%s:%d)\n",
						__FILE__, __LINE__);
				exit(1);
			}
#endif
			free_dir(this);
			this = NULL;
			prev->next = NULL;
			break;
		}
	}
	if (this) {
		/* check mtime and cache entry age */
		if (now - this->add > MAX_CACHE_AGE ||
			0 != strcmp(this->mtime, req->mtime)) {
			free_dir(this);
			this = NULL;
		}
	}
	if (!this) {
		/* add a new cache entry to the list */
		this = malloc(sizeof(struct DIRCACHE));
		this->refcount = 2;
		this->reading = 1;
		INIT_LOCK(this->lock_refcount);
		INIT_LOCK(this->lock_reading);
		INIT_COND(this->wait_reading);
		this->next = dirs;
		dirs = this;
		DO_UNLOCK(lock_dircache);

		strcpy(this->path,  filename);
		strcpy(this->mtime, req->mtime);
		this->add   = now;
		this->html  = ls(now,req->hostname,filename,req->path,&(this->length));

		DO_LOCK(this->lock_reading);
		this->reading = 0;
		BCAST_COND(this->wait_reading);
		DO_UNLOCK(this->lock_reading);
	} else {
		/* add back to the list */
		this->next = dirs;
		dirs = this;
		this->refcount++;
		DO_UNLOCK(lock_dircache);

		DO_LOCK(this->lock_reading);
		if (this->reading)
			WAIT_COND(this->wait_reading,this->lock_reading);
		DO_UNLOCK(this->lock_reading);
	}

	req->body  = this->html;
	req->lbody = this->length;
	return this;
}
