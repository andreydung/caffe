#include <algorithm>
#include <vector>

#include "caffe/loss_layers.hpp"
#include "caffe/util/math_functions.hpp"

namespace caffe {

template <typename Dtype>
void ContrastiveLossLayer<Dtype>::LayerSetUp( const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {

	LossLayer<Dtype>::LayerSetUp(bottom, top);

	CHECK_EQ(bottom[0]->channels(), bottom[1]->channels());
	CHECK_EQ(bottom[0]->height(), bottom[1]->height());
	CHECK_EQ(bottom[0]->width(), bottom[1]->width());
	CHECK_EQ(bottom[2]->channels(), 1);
	CHECK_EQ(bottom[2]->height(), bottom[1]->height());
	CHECK_EQ(bottom[2]->width(), bottom[1]->width());

	diff_.Reshape(bottom[0]->num(), bottom[0]->channels(), bottom[0]->height(), bottom[0]->width());
	diff_sq_.Reshape(bottom[0]->num(), bottom[0]->channels(), bottom[0]->height(), bottom[0]->width());
	dist_sq_.Reshape(bottom[0]->num(), 1, bottom[0]->height(), bottom[1]->width());

	// vector of ones used to sum along channels
	summer_vec_.Reshape(1, 1, 1, bottom[0]->channels());
	for (int i = 0; i < bottom[0]->channels(); ++i) {
		summer_vec_.mutable_cpu_data()[i] = Dtype(1);
	}
	vector<int> loss_shape(0);
	top[0]->Reshape(loss_shape);

}

template <typename Dtype>
void ContrastiveLossLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
	diff_.Reshape(bottom[0]->num(), bottom[0]->channels(), bottom[0]->height(), bottom[0]->width());
	diff_sq_.Reshape(bottom[0]->num(), bottom[0]->channels(), bottom[0]->height(), bottom[0]->width());
	dist_sq_.Reshape(bottom[0]->num(), 1, bottom[0]->height(), bottom[1]->width());

	// vector of ones used to sum along channels
	summer_vec_.Reshape(1, 1, 1, bottom[0]->channels());
	for (int i = 0; i < bottom[0]->channels(); ++i) {
		summer_vec_.mutable_cpu_data()[i] = Dtype(1);
	}
	vector<int> loss_shape(0);
	top[0]->Reshape(loss_shape);
}

template <typename Dtype>
void ContrastiveLossLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top) {
  const int num = bottom[0]->num();
  const int count = bottom[0]->count();
  const int channels = bottom[0]->channels();
  const int dim = bottom[0]->height() * bottom[0]->width();

  Dtype margin = this->layer_param_.contrastive_loss_param().margin();
  Dtype alpha_dissimilar = this->layer_param_.contrastive_loss_param().alpha_dissimilar();
  bool legacy_version = this->layer_param_.contrastive_loss_param().legacy_version();

  caffe_sub(
      count,
      bottom[0]->cpu_data(),  // a
      bottom[1]->cpu_data(),  // b
      diff_.mutable_cpu_data());  // a_i-b_i

  caffe_powx(count, diff_.cpu_data(), Dtype(2), diff_sq_.mutable_cpu_data());

  for (int i = 0; i < num; i++) {
	  caffe_cpu_gemv<Dtype>(CblasTrans, channels, dim, 1.,
		  diff_sq_.cpu_data() + i*channels*dim, summer_vec_.cpu_data(), 0., dist_sq_.mutable_cpu_data() + i*dim);
  }

  Dtype loss(0.0);
  for (int i = 0; i < num*dim; i++) {
	  if (static_cast<int>(bottom[2]->cpu_data()[i])) {  // similar pairs
		  loss += dist_sq_.cpu_data()[i];
	  } else {  // dissimilar pairs
		  if (legacy_version) {
			  loss += alpha_dissimilar * std::max(margin - dist_sq_.cpu_data()[i], Dtype(0.0));
		  } else {
			  Dtype dist = std::max(margin - sqrt(dist_sq_.cpu_data()[i]), 0.0);
			  loss += alpha_dissimilar * dist*dist;
		  }
	  }
  }

  loss = loss / Dtype(dim * num * 2);
  top[0]->mutable_cpu_data()[0] = loss;
}

template <typename Dtype>
void ContrastiveLossLayer<Dtype>::Backward_cpu(	const vector<Blob<Dtype>*>& top,
												const vector<bool>& propagate_down,
												const vector<Blob<Dtype>*>& bottom) {

  Dtype margin = this->layer_param_.contrastive_loss_param().margin();
  Dtype alpha_dissimilar = this->layer_param_.contrastive_loss_param().alpha_dissimilar();
  bool legacy_version = this->layer_param_.contrastive_loss_param().legacy_version();

  const int num = bottom[0]->num();
  const int channels = bottom[0]->channels();
  const int dim = bottom[0]->height() * bottom[0]->width();

  for (int i = 0; i < 2; ++i) {
    if (propagate_down[i]) {

	  Dtype* bout = bottom[i]->mutable_cpu_diff();

      const Dtype sign = (i == 0) ? 1 : -1;
      const Dtype alpha = sign * top[0]->cpu_diff()[0] /
    		  	  	  	  static_cast<Dtype>(num * dim);

      // for similar cases
      // move out of the loop to be quicker
      caffe_cpu_axpby(
                    channels*dim*num,
                    alpha,
                    diff_.cpu_data(),
                    Dtype(0.0),
                    bout);

      for (int j = 0; j < num; ++j) {
    	for(int k = 0; k < dim; k++) {

    		// dissimilar pairs
    		if (!static_cast<int>(bottom[2]->cpu_data()[j * dim + k])) {

    		  Dtype mdist(0.0);
			  Dtype beta(0.0);

			  if (legacy_version) {
				mdist = margin - dist_sq_.cpu_data()[j*dim + k];
				beta = -alpha;
			  }
			  else {
				Dtype dist = sqrt(dist_sq_.cpu_data()[j*dim + k]);
				mdist = margin - dist;
				beta = -alpha * mdist / (dist + Dtype(1e-4));
			  }

			  if (mdist > Dtype(0.0)) {
				for(int c = 0; c < channels; c++) {
					bout[j*channels*dim + c*dim + k] = diff_.cpu_data()[j*channels*dim + c*dim + k] * beta / alpha_dissimilar;
				}

			  } else {
				for(int c = 0; c < channels; c++) {
					bout[j*channels*dim + c*dim + k] = 0;
				}
			  }
			}
    	  }
        }
    }
  }
}

#ifdef CPU_ONLY
STUB_GPU(ContrastiveLossLayer);
#endif

INSTANTIATE_CLASS(ContrastiveLossLayer);
REGISTER_LAYER_CLASS(ContrastiveLoss);

}  // namespace caffe
