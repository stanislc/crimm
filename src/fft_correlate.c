#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <complex.h>
#include <omp.h>
#include <fftw3.h>
#include <Python.h>
#include <numpy/npy_math.h>
#include <numpy/arrayobject.h>

typedef struct {
    int index;
    float score;
} ScoreIndexPair;

typedef struct {
    int x, y, z;
} Dim3d;

// Function to fill indices array with integers from 0 to n
void fill_indices(int *indices, int n) {
    for (int i = 0; i < n; i++) {
        indices[i] = i;
    }
}

// Comparator function for max heap
int comparator(const void *p, const void *q) {
    float l = ((ScoreIndexPair *)p)->score;
    float r = ((ScoreIndexPair *)q)->score;
    return (l < r) - (l > r);
}

void get_top_n_scores(
  float* scores, int n, int top_n_poses, ScoreIndexPair* top_scores
) {
    // Initialize max heap
    ScoreIndexPair* heap = (ScoreIndexPair*) malloc(top_n_poses * sizeof(ScoreIndexPair));

    for (int i = 0; i < n; i++) {
        if (i < top_n_poses) {
            heap[i].index = i;
            heap[i].score = scores[i];
            if (i == top_n_poses - 1) {
                // Build the heap when it's full
                qsort(heap, top_n_poses, sizeof(ScoreIndexPair), comparator);
            }
        } else if (scores[i] < heap[0].score) {
            // Replace the root of the heap
            heap[0].index = i;
            heap[0].score = scores[i];
            // Re-sort the heap
            qsort(heap, top_n_poses, sizeof(ScoreIndexPair), comparator);
        }
    }

    // Copy the heap to the output array
    for (int i = 0; i < top_n_poses; i++) {
        top_scores[i].index = heap[i].index;
        top_scores[i].score = heap[i].score;
    }

    free(heap);
}

// Function to perform batch 3D FFT correlation
void fft_correlate_batch(
  PyArrayObject *recep_grid, PyArrayObject *result, int n_threads
) {
  // recep_grid: (n_grids, nx, ny, nz)
  // result: (n_orientations, n_grids, nx, ny, nz)

  // Get array dimensions
  int n_grids = PyArray_DIMS(recep_grid)[0];
  int n_orientations = PyArray_DIMS(result)[0];
  int nx = PyArray_DIMS(recep_grid)[1];
  int ny = PyArray_DIMS(recep_grid)[2];
  int nz = PyArray_DIMS(recep_grid)[3];
  int N_grid_points = nx * ny * nz;
  // Number FFT coefficients (only half of the array is needed due to symmetry)
  int N_fft_points = nx * ny * (nz / 2 + 1);

  // Get NumPy arrays data pointers
  float *recep_arr = (float *)PyArray_GETPTR1(recep_grid, 0);
  float *lig_arr = (float *)PyArray_GETPTR1(result, 0);

  // Allocate memory for FFTW plans and data
  fftwf_plan plan_fwd, plan_inv;
  fftwf_complex *fft_r, *fft_l;
  fft_r = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * N_fft_points);
  fft_l = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * N_fft_points * n_orientations);

  // Create forward and inverse FFT plans for both arrays
  plan_fwd = fftwf_plan_dft_r2c_3d(nx, ny, nz, recep_arr, fft_r, FFTW_MEASURE);
  plan_inv = fftwf_plan_dft_c2r_3d(nx, ny, nz, fft_l, lig_arr, FFTW_MEASURE);

  float scale = 1.0 / N_grid_points;
  // Execute forward FFTs on both arrays
  for (int i = 0; i < n_grids; i++) {
    float *cur_recep = (float *)PyArray_GETPTR1(recep_grid, i);
    fftwf_execute_dft_r2c(plan_fwd, cur_recep, fft_r);
    #pragma omp parallel for num_threads(n_threads)
    for (int j = 0; j < n_orientations; j++) {
      float *cur_lig = (float *)PyArray_GETPTR2(result, j, i);
      fftwf_complex *cur_fft_l = fft_l + j * N_fft_points;
      // The pointer to the padded ligand array is the same as the pointer 
      // to the correlation array
      fftwf_execute_dft_r2c(plan_fwd, cur_lig, cur_fft_l);
      // Perform element-wise complex conjugate multiplication
      for (int k = 0; k < N_fft_points; k++) {
        cur_fft_l[k] = conjf(fft_r[k]) * cur_fft_l[k] * scale;
      }
      // Execute inverse FFT on the product
      fftwf_execute_dft_c2r(plan_inv, cur_fft_l, cur_lig);
    }
  }

  // Clean up memory and plans
  fftwf_destroy_plan(plan_fwd);
  fftwf_destroy_plan(plan_inv);
  fftwf_free(fft_r);
  fftwf_free(fft_l);
}

// Function to find negative values in an array and store their indices
// Returns the number of negative values
int find_neg_vals(float *arr, int n, int *neg_val_ids) {
    int neg_val_counter = 1; 
    // Start at 1 to avoid overwriting the first index
    // The index 0 is temp place holder for positive values
    int cur_id = 0;
    for (int i = 0; i < n; i++) {
      bool is_neg = signbit(arr[i]); // 1 if negative, 0 if positive
      cur_id = is_neg * neg_val_counter; // 0 if positive, cur_id > 0 if negative
      neg_val_ids[cur_id] = i;
      neg_val_counter += is_neg; // Increment counter if negative
    }
    return neg_val_counter;
}

void flip_and_roll(PyArrayObject *grids, int roll_steps){
  // Get array dimensions
  int n_orientations = PyArray_DIMS(grids)[0];
  int n_grids = PyArray_DIMS(grids)[1];
  int nx = PyArray_DIMS(grids)[2];
  int ny = PyArray_DIMS(grids)[3];
  int nz = PyArray_DIMS(grids)[4];
  int N_grid_points = nx * ny * nz;

  // Flip and roll
  #pragma omp parallel for
  for (int i = 0; i < n_orientations; i++) {
    float *cur_arr = (float *)PyArray_GETPTR1(grids, i);
    for (int j = 0; j < n_grids; j++) {
      float *cur_arr_grids = (float *)PyArray_GETPTR2(grids, i, j);
      int x, y, z;
      for (int k = 0; k < N_grid_points; k++) {
        x = k / (ny * nz);
        y = (k % (ny * nz)) / nz;
        z = k % nz;
        cur_arr_grids[k] = cur_arr[
          (x + roll_steps) % nx * ny * nz + 
          (y + roll_steps) % ny * nz + 
          (z + roll_steps) % nz
        ];
      }
    }
  }
}

int roll_cur_idx(
  int nx, int ny, int nz, int x, int y, int z, int roll_steps
){
  int updated_idx;
  updated_idx = (x + roll_steps) % nx * ny * nz + 
    (y + roll_steps) % ny * nz + 
    (z + roll_steps) % nz;
  return updated_idx;
}

void sum_grids(
  PyArrayObject *grids, PyArrayObject *result, int roll_steps,
  int *neg_val_ids, int *n_neg_vals
) {
  printf("Roll steps: %d\n", roll_steps);
  // Get array dimensions
  int n_orientations = PyArray_DIMS(grids)[0];
  int n_grids = PyArray_DIMS(grids)[1];
  int nx = PyArray_DIMS(grids)[2];
  int ny = PyArray_DIMS(grids)[3];
  int nz = PyArray_DIMS(grids)[4];
  int N_grid_points = nx * ny * nz;

  // Sum all grids
  #pragma omp parallel for
  for (int i = 0; i < n_orientations; i++) {
    float *cur_arr_result = (float *)PyArray_GETPTR1(result, i);
    for (int j = 0; j < n_grids; j++) {
      float *cur_arr_grids = (float *)PyArray_GETPTR2(grids, i, j);
      for (int x = nx - 1, new_x = 0; x >= 0; x--, new_x++) {
        for (int y = ny - 1, new_y = 0; y >= 0; y--, new_y++) {
            for (int z = nz - 1, new_z = 0; z >= 0; z--, new_z++) {
              int updated_idx = (new_x + roll_steps) % nx * ny * nz + 
                (new_y + roll_steps) % ny * nz + 
                (new_z + roll_steps) % nz;
              cur_arr_result[updated_idx] += cur_arr_grids[
                x * ny * nz + y * nz + z
              ];
            }
        }
      }
    }
    // n_neg_vals[i] = find_neg_vals(
    //   cur_arr_result, N_grid_points, neg_val_ids + i * N_grid_points
    // );
  }
}


// Wrapper function to be called from Python
static PyObject* py_fft_correlate_batch(PyObject* self, PyObject* args) {
    PyArrayObject *recep_grid, *result;
    int n_threads;
    if (!PyArg_ParseTuple(
      args, "O!O!i", 
      &PyArray_Type, &recep_grid, 
      &PyArray_Type, &result,
      &n_threads
    )) {
        return NULL;
    }

    // Check array dimensions and data type
    if (PyArray_NDIM(recep_grid) != 4 || PyArray_TYPE(recep_grid) != NPY_FLOAT32) {
      PyErr_SetString(
        PyExc_TypeError, "Expected receptor arrays of float32 with 4 dimensions."
      );
      return NULL;
    }

    if (PyArray_NDIM(result) != 5 || PyArray_TYPE(result) != NPY_FLOAT32) {
      PyErr_SetString(
        PyExc_TypeError, "Expected result arrays of float32 with 5 dimensions."
      );
      return NULL;
    }
    int n_grids = PyArray_DIMS(recep_grid)[0];
    int n_grids_result = PyArray_DIMS(result)[1];
    if (n_grids != n_grids_result) {
      PyErr_SetString(
        PyExc_TypeError, 
        "Expected same number of grids for both receptor and result arrays."
      );
      return NULL;
    }

    fft_correlate_batch(recep_grid, result, n_threads);

    Py_RETURN_NONE;
}

static PyObject* py_sum_grids(PyObject* self, PyObject* args) {
    PyArrayObject *grids, *result;
    int roll_steps;
    if (!PyArg_ParseTuple(
      args, "O!iO!", 
      &PyArray_Type, &grids, 
      &roll_steps,
      &PyArray_Type, &result
    )) {
        return NULL;
    }

    // Check array dimensions and data type
    if (PyArray_NDIM(grids) != 5 || PyArray_TYPE(grids) != NPY_FLOAT32) {
      PyErr_SetString(
        PyExc_TypeError, "Expected grids array of float32 with 5 dimensions."
      );
      return NULL;
    }

    if (PyArray_NDIM(result) != 4 || PyArray_TYPE(result) != NPY_FLOAT32) {
      PyErr_SetString(
        PyExc_TypeError, "Expected result array of float32 with 4 dimensions."
      );
      return NULL;
    }

    int n_orientations = PyArray_DIMS(grids)[0];
    // int n_grids = PyArray_DIMS(grids)[1];
    int nx = PyArray_DIMS(grids)[2];
    int ny = PyArray_DIMS(grids)[3];
    int nz = PyArray_DIMS(grids)[4];
    int N_grid_points = nx * ny * nz;

    int *neg_val_ids = (int *)malloc(n_orientations * N_grid_points * sizeof(int));
    int *n_neg_vals = (int *)malloc(n_orientations * sizeof(int));

    sum_grids(
      grids, result, roll_steps, neg_val_ids, n_neg_vals
    );

    // PyObject *neg_val_ids_out = PyArray_SimpleNewFromData(
    //   1, &n_neg_vals, NPY_INT, neg_val_ids
    // );

    Py_RETURN_NONE;
}

// Method table
static PyMethodDef FftCorrelateMethods[] = {
    {"fft_correlate_batch", py_fft_correlate_batch, METH_VARARGS, "FFT Correlation Batch"},
    {"sum_grids", py_sum_grids, METH_VARARGS, "Sum grids"},
    {NULL, NULL, 0, NULL}
};

// Module definition
static struct PyModuleDef fftcorrelatemodule = {
    PyModuleDef_HEAD_INIT,
    "fft_correlate",   // name of module
    NULL,              // module documentation, may be NULL
    -1,                // size of per-interpreter state of the module, or -1 if the module keeps state in global variables.
    FftCorrelateMethods
};

// Initialization function
PyMODINIT_FUNC PyInit_fft_correlate(void) {
    import_array();
    return PyModule_Create(&fftcorrelatemodule);
}